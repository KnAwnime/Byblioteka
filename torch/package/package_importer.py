import builtins
from contextlib import contextmanager
import importlib
import inspect
import io
import linecache
import os.path
import types
from pathlib import Path
from typing import Any, BinaryIO, Callable, Dict, List, Optional, Union
from weakref import WeakValueDictionary

import torch
from torch.serialization import _get_restore_location, _maybe_decode_ascii

from .file_structure_representation import Directory, _create_directory_from_file_list
from .glob_group import GlobPattern
from ._importlib import (
    _calc___package__,
    _normalize_line_endings,
    _normalize_path,
    _resolve_name,
    _sanity_check,
)
from ._mangling import PackageMangler, demangle
from ._mock_zipreader import MockZipReader
from ._package_unpickler import PackageUnpickler
from .importer import Importer


class PackageImporter(Importer):
    """Importers allow you to load code written to packages by PackageExporter.
    Code is loaded in a hermetic way, using files from the package
    rather than the normal python import system. This allows
    for the packaging of PyTorch model code and data so that it can be run
    on a server or used in the future for transfer learning.

    The importer for packages ensures that code in the module can only be loaded from
    within the package, except for modules explicitly listed as external during export.
    The file `extern_modules` in the zip archive lists all the modules that a package externally depends on.
    This prevents "implicit" dependencies where the package runs locally because it is importing
    a locally-installed package, but then fails when the package is copied to another machine.
    """

    """The dictionary of already loaded modules from this package, equivalent to `sys.modules` but
    local to this importer.
    """
    modules: Dict[str, types.ModuleType]

    def __init__(
        self,
        file_or_buffer: Union[str, torch._C.PyTorchFileReader, Path, BinaryIO],
        module_allowed: Callable[[str], bool] = lambda module_name: True,
    ):
        """Open `file_or_buffer` for importing. This checks that the imported package only requires modules
        allowed by `module_allowed`

        Args:
            file_or_buffer: a file-like object (has to implement :meth:`read`, :meth:`readline`, :meth:`tell`, and :meth:`seek`),
                or a string or os.PathLike object containing a file name.
            module_allowed (Callable[[str], bool], optional): A method to determine if a externally provided module
                should be allowed. Can be used to ensure packages loaded do not depend on modules that the server
                does not support. Defaults to allowing anything.

        Raises:
            ImportError: If the package will use a disallowed module.
        """
        self.zip_reader: Any
        if isinstance(file_or_buffer, torch._C.PyTorchFileReader):
            self.filename = "<pytorch_file_reader>"
            self.zip_reader = file_or_buffer
        elif isinstance(file_or_buffer, (Path, str)):
            self.filename = str(file_or_buffer)
            if not os.path.isdir(self.filename):
                self.zip_reader = torch._C.PyTorchFileReader(self.filename)
            else:
                self.zip_reader = MockZipReader(self.filename)
        else:
            self.filename = "<binary>"
            self.zip_reader = torch._C.PyTorchFileReader(file_or_buffer)

        self.root = _PackageNode(None)
        self.modules = {}
        self.extern_modules = self._read_extern()

        for extern_module in self.extern_modules:
            if not module_allowed(extern_module):
                raise ImportError(
                    f"package '{file_or_buffer}' needs the external module '{extern_module}' "
                    f"but that module has been disallowed"
                )
            self._add_extern(extern_module)

        for fname in self.zip_reader.get_all_records():
            self._add_file(fname)

        self.patched_builtins = builtins.__dict__.copy()
        self.patched_builtins["__import__"] = self.__import__
        # Allow packaged modules to reference their PackageImporter
        self.modules["torch_package_importer"] = self  # type: ignore[assignment]

        self._mangler = PackageMangler()

        # used for reduce deserializaiton
        self.storage_context: Any = None
        self.last_map_location = None

        # used for torch.serialization._load
        self.Unpickler = lambda *args, **kwargs: PackageUnpickler(self, *args, **kwargs)

    def import_module(self, name: str, package=None):
        """Load a module from the package if it hasn't already been loaded, and then return
        the module. Modules are loaded locally
        to the importer and will appear in `self.modules` rather than `sys.modules`

        Args:
            name (str): Fully qualified name of the module to load.
            package ([type], optional): Unused, but present to match the signature of importlib.import_module. Defaults to None.

        Returns:
            types.ModuleType: the (possibly already) loaded module.
        """
        return self._gcd_import(name)

    def load_binary(self, package: str, resource: str) -> bytes:
        """Load raw bytes.

        Args:
            package (str): The name of module package (e.g. "my_package.my_subpackage")
            resource (str): The unique name for the resource.

        Returns:
            bytes: The loaded data.
        """

        path = self._zipfile_path(package, resource)
        return self.zip_reader.get_record(path)

    def load_text(
        self,
        package: str,
        resource: str,
        encoding: str = "utf-8",
        errors: str = "strict",
    ) -> str:
        """Load a string.

        Args:
            package (str): The name of module package (e.g. "my_package.my_subpackage")
            resource (str): The unique name for the resource.
            encoding (str, optional): Passed to `decode`. Defaults to 'utf-8'.
            errors (str, optional): Passed to `decode`. Defaults to 'strict'.

        Returns:
            str: The loaded text.
        """
        data = self.load_binary(package, resource)
        return data.decode(encoding, errors)

    def load_pickle(self, package: str, resource: str, map_location=None) -> Any:
        """Unpickles the resource from the package, loading any modules that are needed to construct the objects
        using :meth:`import_module`

        Args:
            package (str): The name of module package (e.g. "my_package.my_subpackage")
            resource (str): The unique name for the resource.
            map_location: Passed to `torch.load` to determine how tensors are mapped to devices. Defaults to None.

        Returns:
            Any: the unpickled object.
        """
        pickle_file = self._zipfile_path(package, resource)
        restore_location = _get_restore_location(map_location)
        loaded_storages = {}
        loaded_reduces = {}
        storage_context = torch._C.StorageContext()

        def load_tensor(data_type, size, key, location, restore_location):
            name = f"{key}.storage"
            dtype = data_type(0).dtype

            if storage_context.has_storage(name):
                storage = storage_context.get_storage(name, dtype).storage()
            else:
                tensor = self.zip_reader.get_storage_from_record(
                    ".data/" + name, size, dtype
                )
                if isinstance(self.zip_reader, torch._C.PyTorchFileReader):
                    storage_context.add_storage(name, tensor)
                storage = tensor.storage()
            loaded_storages[key] = restore_location(storage, location)

        def persistent_load(saved_id):
            assert isinstance(saved_id, tuple)
            typename = _maybe_decode_ascii(saved_id[0])
            data = saved_id[1:]

            if typename == "storage":
                data_type, key, location, size = data
                if key not in loaded_storages:
                    load_tensor(
                        data_type,
                        size,
                        key,
                        _maybe_decode_ascii(location),
                        restore_location,
                    )
                storage = loaded_storages[key]
                return storage
            elif typename == "reduce_package":
                # to fix BC breaking change, objects on this load path
                # will be loaded multiple times erroneously
                if len(data) == 2:
                    func, args = data
                    return func(self, *args)
                reduce_id, func, args = data
                if reduce_id not in loaded_reduces:
                    loaded_reduces[reduce_id] = func(self, *args)
                return loaded_reduces[reduce_id]
            else:
                f"Unknown typename for persistent_load, expected 'storage' or 'reduce_package' but got '{typename}'"

        # Load the data (which may in turn use `persistent_load` to load tensors)
        data_file = io.BytesIO(self.zip_reader.get_record(pickle_file))
        unpickler = self.Unpickler(data_file)
        unpickler.persistent_load = persistent_load

        @contextmanager
        def set_deserialization_context():
            # to let reduce_package access deserializaiton context
            self.storage_context = storage_context
            self.last_map_location = map_location
            try:
                yield
            finally:
                self.storage_context = None
                self.last_map_location = None

        with set_deserialization_context():
            result = unpickler.load()

        # TODO from zdevito:
        #   This stateful weird function will need to be removed in our efforts
        #   to unify the format. It has a race condition if multiple python
        #   threads try to read independent files
        torch._utils._validate_loaded_sparse_tensors()

        return result

    def id(self):
        """
        Returns internal identifier that torch.package uses to distinguish PackageImporter instances.
        Looks like::

            <torch_package_0>
        """
        return self._mangler.parent_name()

    def file_structure(
        self, *, include: "GlobPattern" = "**", exclude: "GlobPattern" = ()
    ) -> Directory:
        """Returns a file structure representation of package's zipfile.

        Args:
            include (Union[List[str], str]): An optional string e.g. "my_package.my_subpackage", or optional list of strings
                for the names of the files to be inluded in the zipfile representation. This can also be
                a glob-style pattern, as described in :meth:`PackageExporter.mock`

            exclude (Union[List[str], str]): An optional pattern that excludes files whose name match the pattern.

        Returns:
            :class:`Directory`
        """
        return _create_directory_from_file_list(
            self.filename, self.zip_reader.get_all_records(), include, exclude
        )

    def _read_extern(self):
        return (
            self.zip_reader.get_record(".data/extern_modules")
            .decode("utf-8")
            .splitlines(keepends=False)
        )

    def _make_module(
        self, name: str, filename: Optional[str], is_package: bool, parent: str
    ):
        mangled_filename = self._mangler.mangle(filename) if filename else None
        spec = importlib.machinery.ModuleSpec(name, self, is_package=is_package)  # type: ignore[arg-type]
        module = importlib.util.module_from_spec(spec)
        self.modules[name] = module
        module.__name__ = self._mangler.mangle(name)
        ns = module.__dict__
        ns["__spec__"] = spec
        ns["__loader__"] = self
        ns["__file__"] = mangled_filename
        ns["__cached__"] = None
        ns["__builtins__"] = self.patched_builtins
        ns["__torch_package__"] = True

        # Add this module to our private global registry. It should be unique due to mangling.
        assert module.__name__ not in _package_imported_modules
        _package_imported_modules[module.__name__] = module

        # pre-emptively install on the parent to prevent IMPORT_FROM from trying to
        # access sys.modules
        self._install_on_parent(parent, name, module)

        if filename is not None:
            assert mangled_filename is not None
            # pre-emptively install the source in `linecache` so that stack traces,
            # `inspect`, etc. work.
            assert filename not in linecache.cache  # type: ignore[attr-defined]
            linecache.lazycache(mangled_filename, ns)

            code = self._compile_source(filename, mangled_filename)
            exec(code, ns)

        return module

    def _load_module(self, name: str, parent: str):
        cur: _PathNode = self.root
        for atom in name.split("."):
            if not isinstance(cur, _PackageNode) or atom not in cur.children:
                raise ModuleNotFoundError(
                    f'No module named "{name}" in self-contained archive "{self.filename}"'
                    f" and the module is also not in the list of allowed external modules: {self.extern_modules}",
                    name=name,
                )
            cur = cur.children[atom]
            if isinstance(cur, _ExternNode):
                module = self.modules[name] = importlib.import_module(name)
                return module
        return self._make_module(name, cur.source_file, isinstance(cur, _PackageNode), parent)  # type: ignore[attr-defined]

    def _compile_source(self, fullpath: str, mangled_filename: str):
        source = self.zip_reader.get_record(fullpath)
        source = _normalize_line_endings(source)
        return compile(source, mangled_filename, "exec", dont_inherit=True)

    # note: named `get_source` so that linecache can find the source
    # when this is the __loader__ of a module.
    def get_source(self, module_name) -> str:
        # linecache calls `get_source` with the `module.__name__` as the argument, so we must demangle it here.
        module = self.import_module(demangle(module_name))
        return self.zip_reader.get_record(demangle(module.__file__)).decode("utf-8")

    # note: named `get_resource_reader` so that importlib.resources can find it.
    # This is otherwise considered an internal method.
    def get_resource_reader(self, fullname):
        try:
            package = self._get_package(fullname)
        except ImportError:
            return None
        if package.__loader__ is not self:
            return None
        return _PackageResourceReader(self, fullname)

    def _install_on_parent(self, parent: str, name: str, module: types.ModuleType):
        if not parent:
            return
        # Set the module as an attribute on its parent.
        parent_module = self.modules[parent]
        if parent_module.__loader__ is self:  # type: ignore[union-attr]
            setattr(parent_module, name.rpartition(".")[2], module)

    # note: copied from cpython's import code, with call to create module replaced with _make_module
    def _do_find_and_load(self, name):
        path = None
        parent = name.rpartition(".")[0]
        if parent:
            if parent not in self.modules:
                self._gcd_import(parent)
            # Crazy side-effects!
            if name in self.modules:
                return self.modules[name]
            parent_module = self.modules[parent]
            try:
                path = parent_module.__path__  # type: ignore[attr-defined]
            except AttributeError:
                msg = (_ERR_MSG + "; {!r} is not a package").format(name, parent)
                raise ModuleNotFoundError(msg, name=name) from None

        module = self._load_module(name, parent)

        self._install_on_parent(parent, name, module)

        return module

    # note: copied from cpython's import code
    def _find_and_load(self, name):
        module = self.modules.get(name, _NEEDS_LOADING)
        if module is _NEEDS_LOADING:
            return self._do_find_and_load(name)

        if module is None:
            message = "import of {} halted; " "None in sys.modules".format(name)
            raise ModuleNotFoundError(message, name=name)

        return module

    def _gcd_import(self, name, package=None, level=0):
        """Import and return the module based on its name, the package the call is
        being made from, and the level adjustment.

        This function represents the greatest common denominator of functionality
        between import_module and __import__. This includes setting __package__ if
        the loader did not.

        """
        _sanity_check(name, package, level)
        if level > 0:
            name = _resolve_name(name, package, level)

        return self._find_and_load(name)

    # note: copied from cpython's import code
    def _handle_fromlist(self, module, fromlist, *, recursive=False):
        """Figure out what __import__ should return.

        The import_ parameter is a callable which takes the name of module to
        import. It is required to decouple the function from assuming importlib's
        import implementation is desired.

        """
        module_name = demangle(module.__name__)
        # The hell that is fromlist ...
        # If a package was imported, try to import stuff from fromlist.
        if hasattr(module, "__path__"):
            for x in fromlist:
                if not isinstance(x, str):
                    if recursive:
                        where = module_name + ".__all__"
                    else:
                        where = "``from list''"
                    raise TypeError(
                        f"Item in {where} must be str, " f"not {type(x).__name__}"
                    )
                elif x == "*":
                    if not recursive and hasattr(module, "__all__"):
                        self._handle_fromlist(module, module.__all__, recursive=True)
                elif not hasattr(module, x):
                    from_name = "{}.{}".format(module_name, x)
                    try:
                        self._gcd_import(from_name)
                    except ModuleNotFoundError as exc:
                        # Backwards-compatibility dictates we ignore failed
                        # imports triggered by fromlist for modules that don't
                        # exist.
                        if (
                            exc.name == from_name
                            and self.modules.get(from_name, _NEEDS_LOADING) is not None
                        ):
                            continue
                        raise
        return module

    def __import__(self, name, globals=None, locals=None, fromlist=(), level=0):
        if level == 0:
            module = self._gcd_import(name)
        else:
            globals_ = globals if globals is not None else {}
            package = _calc___package__(globals_)
            module = self._gcd_import(name, package, level)
        if not fromlist:
            # Return up to the first dot in 'name'. This is complicated by the fact
            # that 'name' may be relative.
            if level == 0:
                return self._gcd_import(name.partition(".")[0])
            elif not name:
                return module
            else:
                # Figure out where to slice the module's name up to the first dot
                # in 'name'.
                cut_off = len(name) - len(name.partition(".")[0])
                # Slice end needs to be positive to alleviate need to special-case
                # when ``'.' not in name``.
                module_name = demangle(module.__name__)
                return self.modules[module_name[: len(module_name) - cut_off]]
        else:
            return self._handle_fromlist(module, fromlist)

    def _get_package(self, package):
        """Take a package name or module object and return the module.

        If a name, the module is imported.  If the passed or imported module
        object is not a package, raise an exception.
        """
        if hasattr(package, "__spec__"):
            if package.__spec__.submodule_search_locations is None:
                raise TypeError("{!r} is not a package".format(package.__spec__.name))
            else:
                return package
        else:
            module = self.import_module(package)
            if module.__spec__.submodule_search_locations is None:
                raise TypeError("{!r} is not a package".format(package))
            else:
                return module

    def _zipfile_path(self, package, resource=None):
        package = self._get_package(package)
        assert package.__loader__ is self
        name = demangle(package.__name__)
        if resource is not None:
            resource = _normalize_path(resource)
            return f"{name.replace('.', '/')}/{resource}"
        else:
            return f"{name.replace('.', '/')}"

    def _get_or_create_package(
        self, atoms: List[str]
    ) -> "Union[_PackageNode, _ExternNode]":
        cur = self.root
        for i, atom in enumerate(atoms):
            node = cur.children.get(atom, None)
            if node is None:
                node = cur.children[atom] = _PackageNode(None)
            if isinstance(node, _ExternNode):
                return node
            if isinstance(node, _ModuleNode):
                name = ".".join(atoms[:i])
                raise ImportError(
                    f"inconsistent module structure. module {name} is not a package, but has submodules"
                )
            assert isinstance(node, _PackageNode)
            cur = node
        return cur

    def _add_file(self, filename: str):
        """Assembles a Python module out of the given file. Will ignore files in the .data directory.

        Args:
            filename (str): the name of the file inside of the package archive to be added
        """
        *prefix, last = filename.split("/")
        if len(prefix) > 1 and prefix[0] == ".data":
            return
        package = self._get_or_create_package(prefix)
        if isinstance(package, _ExternNode):
            raise ImportError(
                f"inconsistent module structure. package contains a module file {filename}"
                f" that is a subpackage of a module marked external."
            )
        if last == "__init__.py":
            package.source_file = filename
        elif last.endswith(".py"):
            package_name = last[: -len(".py")]
            package.children[package_name] = _ModuleNode(filename)

    def _add_extern(self, extern_name: str):
        *prefix, last = extern_name.split(".")
        package = self._get_or_create_package(prefix)
        if isinstance(package, _ExternNode):
            return  # the shorter extern covers this extern case
        package.children[last] = _ExternNode()


_NEEDS_LOADING = object()
_ERR_MSG_PREFIX = "No module named "
_ERR_MSG = _ERR_MSG_PREFIX + "{!r}"


class _PathNode:
    pass


class _PackageNode(_PathNode):
    def __init__(self, source_file: Optional[str]):
        self.source_file = source_file
        self.children: Dict[str, _PathNode] = {}


class _ModuleNode(_PathNode):
    __slots__ = ["source_file"]

    def __init__(self, source_file: str):
        self.source_file = source_file


class _ExternNode(_PathNode):
    pass


# A private global registry of all modules that have been package-imported.
_package_imported_modules: WeakValueDictionary = WeakValueDictionary()

# `inspect` by default only looks in `sys.modules` to find source files for classes.
# Patch it to check our private registry of package-imported modules as well.
_orig_getfile = inspect.getfile


def patched_getfile(object):
    if inspect.isclass(object):
        if object.__module__ in _package_imported_modules:
            return _package_imported_modules[object.__module__].__file__
    return _orig_getfile(object)


inspect.getfile = patched_getfile


class _PackageResourceReader:
    """Private class used to support PackageImporter.get_resource_reader().

    Confirms to the importlib.abc.ResourceReader interface. Allowed to access
    the innards of PackageImporter.
    """

    def __init__(self, importer, fullname):
        self.importer = importer
        self.fullname = fullname

    def open_resource(self, resource):
        from io import BytesIO

        return BytesIO(self.importer.load_binary(self.fullname, resource))

    def resource_path(self, resource):
        # The contract for resource_path is that it either returns a concrete
        # file system path or raises FileNotFoundError.
        raise FileNotFoundError

    def is_resource(self, name):
        path = self.importer._zipfile_path(self.fullname, name)
        return self.importer.zip_reader.has_record(path)

    def contents(self):
        from pathlib import Path

        filename = self.fullname.replace(".", "/")

        fullname_path = Path(self.importer._zipfile_path(self.fullname))
        files = self.importer.zip_reader.get_all_records()
        subdirs_seen = set()
        for filename in files:
            try:
                relative = Path(filename).relative_to(fullname_path)
            except ValueError:
                continue
            # If the path of the file (which is relative to the top of the zip
            # namespace), relative to the package given when the resource
            # reader was created, has a parent, then it's a name in a
            # subdirectory and thus we skip it.
            parent_name = relative.parent.name
            if len(parent_name) == 0:
                yield relative.name
            elif parent_name not in subdirs_seen:
                subdirs_seen.add(parent_name)
                yield parent_name

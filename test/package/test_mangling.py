from torch.package import PackageExporter, PackageImporter
from torch.package._mangling import (
    PackageMangler,
    demangle,
    get_mangle_prefix,
    is_mangled,
)
from torch.testing._internal.common_utils import run_tests

from io import BytesIO

try:
    from .common import PackageTestCase
except ImportError:
    # Support the case where we run this file directly.
    from common import PackageTestCase


class TestMangling(PackageTestCase):
    def test_unique_manglers(self):
        """
        Each mangler instance should generate a unique mangled name for a given input.
        """
        a = PackageMangler()
        b = PackageMangler()
        self.assertNotEqual(a.mangle("foo.bar"), b.mangle("foo.bar"))

    def test_mangler_is_consistent(self):
        """
        Mangling the same name twice should produce the same result.
        """
        a = PackageMangler()
        self.assertEqual(a.mangle("abc.def"), a.mangle("abc.def"))

    def test_roundtrip_mangling(self):
        a = PackageMangler()
        self.assertEqual("foo", demangle(a.mangle("foo")))

    def test_is_mangled(self):
        a = PackageMangler()
        b = PackageMangler()
        self.assertTrue(is_mangled(a.mangle("foo.bar")))
        self.assertTrue(is_mangled(b.mangle("foo.bar")))

        self.assertFalse(is_mangled("foo.bar"))
        self.assertFalse(is_mangled(demangle(a.mangle("foo.bar"))))

    def test_demangler_multiple_manglers(self):
        """
        PackageDemangler should be able to demangle name generated by any PackageMangler.
        """
        a = PackageMangler()
        b = PackageMangler()

        self.assertEqual("foo.bar", demangle(a.mangle("foo.bar")))
        self.assertEqual("bar.foo", demangle(b.mangle("bar.foo")))

    def test_mangle_empty_errors(self):
        a = PackageMangler()
        with self.assertRaises(AssertionError):
            a.mangle("")

    def test_demangle_base(self):
        """
        Demangling a mangle parent directly should currently return an empty string.
        """
        a = PackageMangler()
        mangled = a.mangle("foo")
        mangle_parent = mangled.partition(".")[0]
        self.assertEqual("", demangle(mangle_parent))

    def test_mangle_prefix(self):
        a = PackageMangler()
        mangled = a.mangle("foo.bar")
        mangle_prefix = get_mangle_prefix(mangled)
        self.assertEqual(mangle_prefix + "." + "foo.bar", mangled)

    def test_unique_module_names(self):
        import package_a.subpackage

        obj = package_a.subpackage.PackageASubpackageObject()
        obj2 = package_a.PackageAObject(obj)
        f1 = BytesIO()
        with PackageExporter(f1, verbose=False) as pe:
            pe.save_pickle("obj", "obj.pkl", obj2)
        f1.seek(0)
        importer1 = PackageImporter(f1)
        loaded1 = importer1.load_pickle("obj", "obj.pkl")
        f1.seek(0)
        importer2 = PackageImporter(f1)
        loaded2 = importer2.load_pickle("obj", "obj.pkl")

        # Modules from loaded packages should not shadow the names of modules.
        # See mangling.md for more info.
        self.assertNotEqual(type(obj2).__module__, type(loaded1).__module__)
        self.assertNotEqual(type(loaded1).__module__, type(loaded2).__module__)


if __name__ == "__main__":
    run_tests()

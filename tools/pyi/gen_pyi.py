from __future__ import print_function
import os
import collections
import glob
import yaml
import re
import argparse

from ..autograd.utils import YamlLoader, CodeTemplate, write
from ..autograd.gen_python_functions import get_py_torch_functions, get_py_variable_methods
from ..autograd.gen_autograd import load_aten_declarations

"""
This module implements generation of type stubs for PyTorch,
enabling use of autocomplete in IDEs like PyCharm, which otherwise
don't understand C extension modules.

At the moment, this module only handles type stubs for torch and
torch.Tensor.  It should eventually be expanded to cover all functions
which come are autogenerated.

Here's our general strategy:

- We start off with a hand-written __init__.pyi.in file.  This
  file contains type definitions for everything we cannot automatically
  generate, including pure Python definitions directly in __init__.py
  (the latter case should be pretty rare).

- We go through automatically bound functions based on the
  type information recorded in Declarations.yaml and
  generate type hints for them (generate_type_hints)

There are a number of type hints which we've special-cased;
read gen_pyi for the gory details.
"""

# TODO: Consider defining some aliases for our Union[...] types, to make
# the stubs to read on the human eye.

needed_modules = set()

FACTORY_PARAMS = "dtype: Optional[_dtype]=None, device: Union[_device, str, None]=None, requires_grad: bool=False"

# this could be more precise w.r.t list contents etc. How to do Ellipsis?
INDICES = "indices: Union[None, _int, slice, Tensor, List, Tuple]"

blacklist = [
    '__init_subclass__',
    '__new__',
    '__subclasshook__',
    'clamp',
    'clamp_',
    'device',
    'grad',
    'requires_grad',
    'range',
    # defined in functional
    'einsum',
    # reduction argument; these bindings don't make sense
    'binary_cross_entropy_with_logits',
    'ctc_loss',
    'cosine_embedding_loss',
    'hinge_embedding_loss',
    'kl_div',
    'margin_ranking_loss',
    'triplet_margin_loss',
    # Somehow, these are defined in both _C and in functional. Ick!
    'broadcast_tensors',
    'meshgrid',
    'cartesian_prod',
    'norm',
    'chain_matmul',
    'stft',
    'tensordot',
    'norm',
    'split',
    'unique_consecutive',
    # These are handled specially by python_arg_parser.cpp
    'add',
    'add_',
    'add_out',
    'sub',
    'sub_',
    'sub_out',
    'mul',
    'mul_',
    'mul_out',
    'div',
    'div_',
    'div_out',
]


def type_to_python(typename, size=None):
    """type_to_python(typename: str, size: str) -> str

    Transforms a Declarations.yaml type name into a Python type specification
    as used for type hints.
    """
    typename = typename.replace(' ', '')  # normalize spaces, e.g., 'Generator *'

    # Disambiguate explicitly sized int/tensor lists from implicitly
    # sized ones.  These permit non-list inputs too.  (IntArrayRef[] and
    # TensorList[] are not real types; this is just for convenience.)
    if typename in {'IntArrayRef', 'TensorList'} and size is not None:
        typename += '[]'

    typename = {
        'Device': 'Union[_device, str, None]',
        'Generator*': 'Generator',
        'IntegerTensor': 'Tensor',
        'Scalar': 'Number',
        'ScalarType': '_dtype',
        'Storage': 'Storage',
        'BoolTensor': 'Tensor',
        'IndexTensor': 'Tensor',
        'Tensor': 'Tensor',
        'MemoryFormat': 'memory_format',
        'IntArrayRef': '_size',
        'IntArrayRef[]': 'Union[_int, _size]',
        'TensorList': 'Union[Tuple[Tensor, ...], List[Tensor]]',
        'TensorList[]': 'Union[Tensor, Tuple[Tensor, ...], List[Tensor]]',
        'bool': 'bool',
        'double': '_float',
        'int64_t': '_int',
        'accreal': 'Number',
        'real': 'Number',
        'void*': '_int',    # data_ptr
        'void': 'None',
        'std::string': 'str',
        'Dimname': 'Union[str, None]',
        'DimnameList': 'List[Union[str, None]]',
        'QScheme': '_qscheme',
    }[typename]

    return typename


def arg_to_type_hint(arg):
    """arg_to_type_hint(arg) -> str

    This takes one argument in a Declarations and returns a string
    representing this argument in a type hint signature.
    """
    name = arg['name']
    if name == 'from':  # from is a Python keyword...
        name += '_'
    typename = type_to_python(arg['dynamic_type'], arg.get('size'))
    if arg.get('is_nullable'):
        typename = 'Optional[' + typename + ']'
    if 'default' in arg:
        default = arg['default']
        if default == 'nullptr':
            default = None
        elif default == 'c10::nullopt':
            default = None
        elif isinstance(default, str) and default.startswith('{') and default.endswith('}'):
            if arg['dynamic_type'] == 'Tensor' and default == '{}':
                default = None
            elif arg['dynamic_type'] == 'IntArrayRef':
                default = '(' + default[1:-1] + ')'
            else:
                raise Exception("Unexpected default constructor argument of type {}".format(arg['dynamic_type']))
        elif default == 'MemoryFormat::Contiguous':
            default = 'contiguous_format'
        elif default == 'QScheme::PER_TENSOR_AFFINE':
            default = 'per_tensor_affine'
        default = '={}'.format(default)
    else:
        default = ''
    return name + ': ' + typename + default


binary_ops = ('add', 'sub', 'mul', 'div', 'pow', 'lshift', 'rshift', 'mod', 'truediv',
              'matmul', 'floordiv',
              'radd', 'rmul', 'rfloordiv',          # reverse arithmetic
              'and', 'or', 'xor',                   # logic
              'iadd', 'iand', 'idiv', 'ilshift', 'imul',
              'ior', 'irshift', 'isub', 'itruediv', 'ixor',  # inplace ops
              )
comparison_ops = ('eq', 'ne', 'ge', 'gt', 'lt', 'le')
unary_ops = ('neg', 'abs', 'invert')
to_py_type_ops = ('bool', 'float', 'long', 'index', 'int', 'nonzero')
all_ops = binary_ops + comparison_ops + unary_ops + to_py_type_ops


def sig_for_ops(opname):
    """sig_for_ops(opname : str) -> List[str]

    Returns signatures for operator special functions (__add__ etc.)"""

    # we have to do this by hand, because they are hand-bound in Python

    assert opname.endswith('__') and opname.startswith('__'), "Unexpected op {}".format(opname)

    name = opname[2:-2]
    if name in binary_ops:
        return ['def {}(self, other: Any) -> Tensor: ...'.format(opname)]
    elif name in comparison_ops:
        # unsafe override https://github.com/python/mypy/issues/5704
        return ['def {}(self, other: Any) -> Tensor: ...  # type: ignore'.format(opname)]
    elif name in unary_ops:
        return ['def {}(self) -> Tensor: ...'.format(opname)]
    elif name in to_py_type_ops:
        if name in {'bool', 'float'}:
            tname = name
        elif name == 'nonzero':
            tname = 'bool'
        else:
            tname = 'int'
        if tname in {'float', 'int'}:
            tname = 'builtins.' + tname
        return ['def {}(self) -> {}: ...'.format(opname, tname)]
    else:
        raise Exception("unknown op", opname)


def generate_type_hints(fname, decls, is_tensor=False):
    """generate_type_hints(fname, decls, is_tensor=False)

    Generates type hints for the declarations pertaining to the function
    :attr:`fname`. attr:`decls` are the declarations from the parsed
    Declarations.yaml.
    The :attr:`is_tensor` flag indicates whether we are parsing
    members of the Tensor class (true) or functions in the
    `torch` namespace (default, false).

    This function currently encodes quite a bit about the semantics of
    the translation C++ -> Python.
    """
    if fname in blacklist:
        return []

    type_hints = []
    dnames = ([d['name'] for d in decls])
    has_out = fname + '_out' in dnames

    if has_out:
        decls = [d for d in decls if d['name'] != fname + '_out']

    for decl in decls:
        render_kw_only_separator = True  # whether we add a '*' if we see a keyword only argument
        python_args = []

        has_tensor_options = 'TensorOptions' in [a['dynamic_type'] for a in decl['arguments']]

        for a in decl['arguments']:
            if a['dynamic_type'] != 'TensorOptions':
                if a.get('kwarg_only', False) and render_kw_only_separator:
                    python_args.append('*')
                    render_kw_only_separator = False
                try:
                    python_args.append(arg_to_type_hint(a))
                except Exception:
                    print("Error while processing function {}".format(fname))
                    raise

        if is_tensor:
            if 'self: Tensor' in python_args:
                python_args.remove('self: Tensor')
                python_args = ['self'] + python_args
            else:
                raise Exception("method without self is unexpected")

        if has_out:
            if render_kw_only_separator:
                python_args.append('*')
                render_kw_only_separator = False
            python_args.append('out: Optional[Tensor]=None')

        if has_tensor_options:
            if render_kw_only_separator:
                python_args.append('*')
                render_kw_only_separator = False
            python_args += ["dtype: _dtype=None",
                            "layout: layout=strided",
                            "device: Union[_device, str, None]=None",
                            "requires_grad:bool=False"]

        python_args_s = ', '.join(python_args)
        python_returns = [type_to_python(r['dynamic_type']) for r in decl['returns']]

        if len(python_returns) > 1:
            python_returns_s = 'Tuple[' + ', '.join(python_returns) + ']'
        else:
            python_returns_s = python_returns[0]

        type_hint = "def {}({}) -> {}: ...".format(fname, python_args_s, python_returns_s)
        numargs = len(decl['arguments'])
        vararg_pos = int(is_tensor)
        have_vararg_version = (numargs > vararg_pos and
                               decl['arguments'][vararg_pos]['dynamic_type'] in {'IntArrayRef', 'TensorList'} and
                               (numargs == vararg_pos + 1 or python_args[vararg_pos + 1] == '*') and
                               (not is_tensor or decl['arguments'][0]['name'] == 'self'))

        type_hints.append(type_hint)

        if have_vararg_version:
            # Two things come into play here: PyTorch has the "magic" that if the first and only positional argument
            # is an IntArrayRef or TensorList, it will be used as a vararg variant.
            # The following outputs the vararg variant, the "pass a list variant" is output above.
            # The other thing is that in Python, the varargs are annotated with the element type, not the list type.
            typelist = decl['arguments'][vararg_pos]['dynamic_type']
            if typelist == 'IntArrayRef':
                vararg_type = '_int'
            else:
                vararg_type = 'Tensor'
            # replace first argument and eliminate '*' if present
            python_args = ((['self'] if is_tensor else []) + ['*' + decl['arguments'][vararg_pos]['name'] +
                                                              ': ' + vararg_type] + python_args[vararg_pos + 2:])
            python_args_s = ', '.join(python_args)
            type_hint = "def {}({}) -> {}: ...".format(fname, python_args_s, python_returns_s)
            type_hints.append(type_hint)

    return type_hints

def gen_nn_modules(out):
    def replace_forward(m):
        # We instruct mypy to not emit errors for the `forward` and `__call__` declarations since mypy
        # would otherwise correctly point out that Module's descendants' `forward` declarations
        # conflict with `Module`s. Specificlaly, `Module` defines `forward(self, *args)` while the
        # descandantes define more specific forms, such as `forward(self, input: Tensor)`, which
        # violates Liskov substitutability. The 'mypy' team recommended this solution for now.
        forward_def = m.group(0) + "  # type: ignore"
        call_def = re.sub(r'def forward', 'def __call__', forward_def)
        new_def = "{}\n{}".format(forward_def, call_def)
        return new_def
    pattern = re.compile(r'^\s*def forward\(self.*$', re.MULTILINE)
    for fname in glob.glob("torch/nn/modules/*.pyi.in"):
        with open(fname, 'r') as f:
            src = f.read()
        res = pattern.sub(replace_forward, src)
        fname_out = fname[:-3]
        with open(os.path.join(out, fname_out), 'w') as f:
            f.write(res)

def gen_nn_functional(out):
    # Functions imported into `torch.nn.functional` from `torch`, perhaps being filtered
    # through an `_add_docstr` call
    imports = [
        'conv1d',
        'conv2d',
        'conv3d',
        'conv_transpose1d',
        'conv_transpose2d',
        'conv_transpose3d',
        'conv_tbc',
        'avg_pool1d',
        'relu_',
        'selu_',
        'celu_',
        'rrelu_',
        'pixel_shuffle',
        'pdist',
        'cosine_similarity',
    ]
    # Functions generated by `torch._jit_internal.boolean_dispatch`
    dispatches = [
        'fractional_max_pool2d',
        'fractional_max_pool3d',
        'max_pool1d',
        'max_pool2d',
        'max_pool3d',
        'adaptive_max_pool1d',
        'adaptive_max_pool2d',
        'adaptive_max_pool3d',
    ]
    # Functions directly imported from `torch._C`
    from_c = [
        'avg_pool2d',
        'avg_pool3d',
        'hardtanh_',
        'elu_',
        'leaky_relu_',
        'logsigmoid',
        'softplus',
        'softshrink',
        'one_hot',
    ]
    import_code = ["from .. import {0} as {0}".format(_) for _ in imports]
    # TODO make these types more precise
    dispatch_code = ["{}: Callable".format(_) for _ in (dispatches + from_c)]
    stubs = CodeTemplate.from_file(os.path.join('torch', 'nn', 'functional.pyi.in'))
    env = {
        'imported_hints': import_code,
        'dispatched_hints': dispatch_code
    }
    write(out, 'torch/nn/functional.pyi', stubs, env)

def gen_nn_pyi(out):
    gen_nn_functional(out)
    gen_nn_modules(out)

def gen_pyi(declarations_path, out):
    """gen_pyi()

    This function generates a pyi file for torch.
    """

    # Some of this logic overlaps with generate_python_signature in
    # tools/autograd/gen_python_functions.py; however, this
    # function is all about generating mypy type signatures, whereas
    # the other function generates are custom format for argument
    # checking.  If you are update this, consider if your change
    # also needs to update the other file.

    # Load information from YAML
    declarations = load_aten_declarations(declarations_path)

    # Generate type signatures for top-level functions
    # ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    unsorted_function_hints = collections.defaultdict(list)
    unsorted_function_hints.update({
        'set_flush_denormal': ['def set_flush_denormal(mode: bool) -> bool: ...'],
        'get_default_dtype': ['def get_default_dtype() -> _dtype: ...'],
        'from_numpy': ['def from_numpy(ndarray) -> Tensor: ...'],
        'clamp': ["def clamp(self, min: _float=-inf, max: _float=inf,"
                  " *, out: Optional[Tensor]=None) -> Tensor: ..."],
        'as_tensor': ["def as_tensor(data: Any, dtype: _dtype=None, device: Optional[_device]=None) -> Tensor: ..."],
        'get_num_threads': ['def get_num_threads() -> _int: ...'],
        'set_num_threads': ['def set_num_threads(num: _int) -> None: ...'],
        'get_num_interop_threads': ['def get_num_interop_threads() -> _int: ...'],
        'set_num_interop_threads': ['def set_num_interop_threads(num: _int) -> None: ...'],
        # These functions are explicitly disabled by
        # SKIP_PYTHON_BINDINGS because they are hand bound.
        # Correspondingly, we must hand-write their signatures.
        'tensor': ["def tensor(data: Any, {}) -> Tensor: ...".format(FACTORY_PARAMS)],
        'sparse_coo_tensor': ['def sparse_coo_tensor(indices: Tensor, values: Union[Tensor,List],'
                              ' size: Optional[_size]=None, *, dtype: Optional[_dtype]=None,'
                              ' device: Union[_device, str, None]=None, requires_grad:bool=False) -> Tensor: ...'],
        'range': ['def range(start: Number, end: Number,'
                  ' step: Number=1, *, out: Optional[Tensor]=None, {}) -> Tensor: ...'
                  .format(FACTORY_PARAMS)],
        'arange': ['def arange(start: Number, end: Number, step: Number, *,'
                   ' out: Optional[Tensor]=None, {}) -> Tensor: ...'
                   .format(FACTORY_PARAMS),
                   'def arange(start: Number, end: Number, *, out: Optional[Tensor]=None, {}) -> Tensor: ...'
                   .format(FACTORY_PARAMS),
                   'def arange(end: Number, *, out: Optional[Tensor]=None, {}) -> Tensor: ...'
                   .format(FACTORY_PARAMS)],
        'randint': ['def randint(low: _int, high: _int, size: _size, *, {}) -> Tensor: ...'
                    .format(FACTORY_PARAMS),
                    'def randint(high: _int, size: _size, *, {}) -> Tensor: ...'
                    .format(FACTORY_PARAMS)],
    })
    for binop in ['add', 'sub', 'mul', 'div']:
        unsorted_function_hints[binop].append(
            'def {}(input: Union[Tensor, Number],'
            ' other: Union[Tensor, Number],'
            ' *, out: Optional[Tensor]=None) -> Tensor: ...'.format(binop))
        unsorted_function_hints[binop].append(
            'def {}(input: Union[Tensor, Number],'
            ' value: Number,'
            ' other: Union[Tensor, Number],'
            ' *, out: Optional[Tensor]=None) -> Tensor: ...'.format(binop))

    function_declarations = get_py_torch_functions(declarations)
    for name in sorted(function_declarations.keys()):
        unsorted_function_hints[name] += generate_type_hints(name, function_declarations[name])

    # Generate type signatures for deprecated functions

    # TODO: Maybe we shouldn't generate type hints for deprecated
    # functions :)  However, examples like those addcdiv rely on these.
    with open('tools/autograd/deprecated.yaml', 'r') as f:
        deprecated = yaml.load(f, Loader=YamlLoader)
    for d in deprecated:
        name, sig = re.match(r"^([^\(]+)\(([^\)]*)", d['name']).groups()
        sig = ['*' if p.strip() == '*' else p.split() for p in sig.split(',')]
        sig = ['*' if p == '*' else (p[1] + ': ' + type_to_python(p[0])) for p in sig]
        unsorted_function_hints[name].append("def {}({}) -> Tensor: ...".format(name, ', '.join(sig)))

    function_hints = []
    for name, hints in sorted(unsorted_function_hints.items()):
        if len(hints) > 1:
            hints = ['@overload\n' + h for h in hints]
        function_hints += hints

    # Generate type signatures for Tensor methods
    # ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    unsorted_tensor_method_hints = collections.defaultdict(list)
    unsorted_tensor_method_hints.update({
        'size': ['def size(self) -> Size: ...',
                 'def size(self, _int) -> _int: ...'],
        'stride': ['def stride(self) -> Tuple[_int]: ...',
                   'def stride(self, _int) -> _int: ...'],
        'new_empty': ['def new_empty(self, size: {}, {}) -> Tensor: ...'.
                      format(type_to_python('IntArrayRef'), FACTORY_PARAMS)],
        'new_ones': ['def new_ones(self, size: {}, {}) -> Tensor: ...'.
                     format(type_to_python('IntArrayRef'), FACTORY_PARAMS)],
        'new_zeros': ['def new_zeros(self, size: {}, {}) -> Tensor: ...'.
                      format(type_to_python('IntArrayRef'), FACTORY_PARAMS)],
        'new_full': ['def new_full(self, size: {}, value: {}, {}) -> Tensor: ...'.
                     format(type_to_python('IntArrayRef'), type_to_python('Scalar'), FACTORY_PARAMS)],
        'new_tensor': ["def new_tensor(self, data: Any, {}) -> Tensor: ...".format(FACTORY_PARAMS)],
        # clamp has no default values in the Declarations
        'clamp': ["def clamp(self, min: _float=-inf, max: _float=inf,"
                  " *, out: Optional[Tensor]=None) -> Tensor: ..."],
        'clamp_': ["def clamp_(self, min: _float=-inf, max: _float=inf) -> Tensor: ..."],
        '__getitem__': ["def __getitem__(self, {}) -> Tensor: ...".format(INDICES)],
        '__setitem__': ["def __setitem__(self, {}, val: Union[Tensor, Number])"
                        " -> None: ...".format(INDICES)],
        'tolist': ['def tolist(self) -> List: ...'],
        'requires_grad_': ['def requires_grad_(self, mode: bool=True) -> Tensor: ...'],
        'element_size': ['def element_size(self) -> _int: ...'],
        'dim': ['def dim(self) -> _int: ...'],
        'ndimension': ['def ndimension(self) -> _int: ...'],
        'nelement': ['def nelement(self) -> _int: ...'],
        'cuda': ['def cuda(self, device: Optional[_device]=None, non_blocking: bool=False) -> Tensor: ...'],
        'numpy': ['def numpy(self) -> Any: ...'],
        'apply_': ['def apply_(self, callable: Callable) -> Tensor: ...'],
        'map_': ['def map_(tensor: Tensor, callable: Callable) -> Tensor: ...'],
        'storage': ['def storage(self) -> Storage: ...'],
        'type': ['def type(self, dtype: Union[None, str, _dtype]=None, non_blocking: bool=False)'
                 ' -> Union[str, Tensor]: ...'],
        'get_device': ['def get_device(self) -> _int: ...'],
        'contiguous': ['def contiguous(self) -> Tensor: ...'],
        'is_contiguous': ['def is_contiguous(self) -> bool: ...'],
        'is_cuda': ['is_cuda: bool'],
        'is_leaf': ['is_leaf: bool'],
        'storage_offset': ['def storage_offset(self) -> _int: ...'],
        'to': ['def to(self, dtype: _dtype, non_blocking: bool=False, copy: bool=False) -> Tensor: ...',
               'def to(self, device: Optional[Union[_device, str]]=None, dtype: Optional[_dtype]=None, '
               'non_blocking: bool=False, copy: bool=False) -> Tensor: ...',
               'def to(self, other: Tensor, non_blocking: bool=False, copy: bool=False) -> Tensor: ...',
               ],
        'item': ["def item(self) -> Number: ..."],
    })
    for binop in ['add', 'sub', 'mul', 'div']:
        for inplace in [True, False]:
            out_suffix = ', *, out: Optional[Tensor]=None'
            if inplace:
                name += '_'
                out_suffix = ''
            unsorted_tensor_method_hints[name].append(
                'def {}(self, other: Union[Tensor, Number]{})'
                ' -> Tensor: ...'.format(name, out_suffix))
            unsorted_tensor_method_hints[name].append(
                'def {}(self, value: Number,'
                ' other: Union[Tensor, Number]{})'
                ' -> Tensor: ...'.format(name, out_suffix))
    simple_conversions = ['byte', 'char', 'cpu', 'double', 'float',
                          'half', 'int', 'long', 'short', 'bool']
    for name in simple_conversions:
        unsorted_tensor_method_hints[name].append('def {}(self) -> Tensor: ...'.format(name))

    tensor_method_declarations = get_py_variable_methods(declarations)
    for name in sorted(tensor_method_declarations.keys()):
        unsorted_tensor_method_hints[name] += \
            generate_type_hints(name, tensor_method_declarations[name], is_tensor=True)

    for op in all_ops:
        name = '__{}__'.format(op)
        unsorted_tensor_method_hints[name] += sig_for_ops(name)

    tensor_method_hints = []
    for name, hints in sorted(unsorted_tensor_method_hints.items()):
        if len(hints) > 1:
            hints = ['@overload\n' + h for h in hints]
        tensor_method_hints += hints

    # TODO: Missing type hints for nn

    # Generate type signatures for legacy classes
    # ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    # TODO: These are deprecated, maybe we shouldn't type hint them
    legacy_class_hints = []
    for c in ('DoubleStorage', 'FloatStorage', 'LongStorage', 'IntStorage',
              'ShortStorage', 'CharStorage', 'ByteStorage', 'BoolStorage'):
        legacy_class_hints.append('class {}(Storage): ...'.format(c))

    for c in ('DoubleTensor', 'FloatTensor', 'LongTensor', 'IntTensor',
              'ShortTensor', 'CharTensor', 'ByteTensor', 'BoolTensor'):
        legacy_class_hints.append('class {}(Tensor): ...'.format(c))

    # Generate type signatures for dtype classes
    # ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    # TODO: don't explicitly list dtypes here; get it from canonical
    # source
    dtype_class_hints = ['{}: dtype = ...'.format(n)
                         for n in
                         ['float32', 'float', 'float64', 'double', 'float16', 'half',
                          'uint8', 'int8', 'int16', 'short', 'int32', 'int', 'int64', 'long',
                          'complex32', 'complex64', 'complex128', 'quint8', 'qint8', 'qint32', 'bool']]

    # Write out the stub
    # ~~~~~~~~~~~~~~~~~~

    env = {
        'function_hints': function_hints,
        'tensor_method_hints': tensor_method_hints,
        'legacy_class_hints': legacy_class_hints,
        'dtype_class_hints': dtype_class_hints,
    }
    TORCH_TYPE_STUBS = CodeTemplate.from_file(os.path.join('torch', '__init__.pyi.in'))

    write(out, 'torch/__init__.pyi', TORCH_TYPE_STUBS, env)
    gen_nn_pyi(out)


def main():
    parser = argparse.ArgumentParser(
        description='Generate type stubs for PyTorch')
    parser.add_argument('--declarations-path', metavar='DECL',
                        default='torch/share/ATen/Declarations.yaml',
                        help='path to Declarations.yaml')
    parser.add_argument('--out', metavar='OUT',
                        default='.',
                        help='path to output directory')
    args = parser.parse_args()
    gen_pyi(args.declarations_path, args.out)


if __name__ == '__main__':
    main()

from itertools import chain
from collections import OrderedDict

import torch
from ..backends.thnn import backend as thnn_backend
from ..parameter import Parameter
from torch.autograd import Variable


class Module(object):
    """This is the base class for all Modules defined in the nn package.
    Even the Container class derives from this class.

    An nn.Module has the following interface:

    **Constructor:**
       nn.Module()

    **forward(...)**

    This is the function that one defines when subclassing to create
    their own modules.
    It takes in inputs and returns outputs.

    **__call__(...)**

    This calls the forward function, as well as the hooks

    **register_parameter(name, param)**

    Adds a parameter to the module. The parameter can be accessed as an
    attribute of the module by its name.

    **register_buffer(name, tensor)**

    This is typically used to register a buffer that is not a Parameter.
    For example, in BatchNorm, the running_mean is a buffer, so one would
    register it in the constructor of BatchNorm with:

    `self.register_buffer('running_mean', torch.zeros(num_features))`

    The registered buffers can simply be accessed as class members
    when needed.

    **cpu()**

    Recursively moves all it's parameters and buffers to the CPU

    **cuda(device_id=None)**
    Recursively moves all it's parameters and buffers to the CUDA memory.
    If device_id is given, moves it to GPU number device_id

    **float()**
    Typecasts the parameters and buffers to float

    **double()**
    Typecasts the parameters and buffers to double

    **register_forward_hook(name, hook)**

    This will register a user-defined closure on the module.
    Whenever the module finishes it's forward operation,
    the user closure is called.
    The signature of the closure is `def closure(input, output)`

    **register_backward_hook(name, hook)**

    This will register a user-defined closure on the module.
    Whenever the module finishes it's backward operation,
    the user closure is called.
    The signature of the closure is `def closure(gradOutput, gradInput)`

    **remove_forward_hook(name)**

    Removes a registered forward hook with the given name

    **remove_backward_hook(name)**

    Removes a registered backward hook with the given name

    **`[generator] parameters()`**

    returns a generator over all learnable parameters in the container instance.
    This can typically be passed to the optimizer API

    ```python
    # .parameters()
    >>> for param in model.parameters():
    >>>     print(type(param.data), param.size())
    <class 'torch.FloatTensor'> (20L,)
    <class 'torch.FloatTensor'> (20L, 1L, 5L, 5L)
    ```

    **`[dict] state_dict()`**

    returns a dictionary of learnable parameters of the Module.
    For example: ['weight' : Parameter(torch.FloatTensor(20x1x5x5)),
                  'bias'   : Parameter(torch.FloatTensor(20)),
                 ]

    ```python
    # .state_dict()
    >>> pdict = model.state_dict()
    >>> print(pdict.keys())
    ['bias', 'weight']
    ```

    **`load_state_dict(dict)`**

    Given a parameter dict, sets the parameters of self to be the given dict.

    **`train()`**

    Sets the Container to training mode (for modules such as batchnorm, dropout etc.)

    **`eval()`**

    Sets the Container to evaluate mode (for modules such as batchnorm, dropout etc.)

    **`zero_grad()`**

    Zeroes the gradients of each Parameter of the module

    """
    def __init__(self):
        self._backend = thnn_backend
        self._parameters = OrderedDict()
        self._buffers = OrderedDict()
        self._backward_hooks = OrderedDict()
        self._forward_hooks = OrderedDict()
        self.training = True
        for name, param in self._parameters.items():
            if not isinstance(param, Parameter):
                if isinstance(param, Variable):
                    raise TypeError("can't use a Variable as a module "
                        "parameter.  Convert it to torch.nn.Parameter first.")
                if param is not None:
                    param = Parameter(param)
            self._parameters[name] = param

    def forward(self, *input):
        raise NotImplementedError

    def register_buffer(self, name, tensor):
        self._buffers[name] = tensor

    def register_parameter(self, name, param):
        if '_parameters' not in self.__dict__:
            raise AttributeError(
                "cannot assign parameter before Module.__init__() call")
        if param is None:
            self._parameters[name] = None
        elif not isinstance(param, Parameter):
            raise TypeError("cannot assign '{}' object to parameter '{}' "
                            "(torch.nn.Parameter or None required)"
                            .format(torch.typename(param), name))
        elif param.creator:
            raise ValueError(
                "Cannot assign non-leaf Variable to parameter '{0}'. Model "
                "parameters must be created explicitly. To express '{0}' "
                "as a function of another variable, compute the value in "
                "the forward() method.".format(name))
        else:
            self._parameters[name] = param

    def _apply(self, fn):
        for param in self._parameters.values():
            if param is not None:
                # Variables stored in modules are graph leaves, and we don't
                # want to create copy nodes, so we have to unpack the data.
                param.data = fn(param.data)
                if param.grad is not None:
                    param._grad = fn(param._grad)

        for key, buf in self._buffers.items():
            if buf is not None:
                self._buffers[key] = fn(buf)
        return self

    def apply(self, fn):
        fn(self)
        return self

    def cuda(self, device_id=None):
        return self._apply(lambda t: t.cuda(device_id))

    def cpu(self, device_id=None):
        return self._apply(lambda t: t.cpu())

    def type(self, dst_type):
        return self._apply(lambda t: t.type(dst_type))

    def float(self):
        return self._apply(lambda t: t.float())

    def double(self):
        return self._apply(lambda t: t.double())

    def register_backward_hook(self, name, hook):
        assert name not in self._backward_hooks, \
            "Trying to register a second backward hook with name {}".format(name)
        self._backward_hooks[name] = lambda gi, go: hook(self, gi, go)

    def remove_backward_hook(self, name):
        assert name in self._backward_hooks, \
            "Trying to remove an inexistent backward hook with name {}".format(name)
        del self._backward_hooks[name]

    def register_forward_hook(self, name, hook):
        assert name not in self._forward_hooks, \
            "Trying to register a second forward hook with name {}".format(name)
        self._forward_hooks[name] = hook

    def remove_forward_hook(self, name):
        assert name in self._forward_hooks, \
            "Trying to remove an inexistent forward hook with name {}".format(name)
        del self._forward_hooks[name]

    def __call__(self, *input, **kwargs):
        result = self.forward(*input, **kwargs)
        for name, hook in self._forward_hooks.items():
            hook_result = hook(self, input, result)
            if hook_result is not None:
                raise RuntimeError("forward hooks should never return any "
                        "values, but '{}' didn't return None".format(name))
        var = result
        while not isinstance(var, Variable):
            var = var[0]
        creator = var.creator
        if creator is not None:
            creator._backward_hooks = self._backward_hooks
        return result

    def __getattr__(self, name):
        if '_parameters' in self.__dict__:
            _parameters = self.__dict__['_parameters']
            if name in _parameters:
                return _parameters[name]
        if '_buffers' in self.__dict__:
            _buffers = self.__dict__['_buffers']
            if name in _buffers:
                return _buffers[name]
        return object.__getattribute__(self, name)

    def __setattr__(self, name, value):
        params = self.__dict__.get('_parameters')
        if isinstance(value, Parameter) or (params and name in params):
            self.register_parameter(name, value)
        else:
            object.__setattr__(self, name, value)

    def __delattr__(self, name):
        if name in self._parameters:
            del self._parameters[name]
        else:
            object.__delattr__(self, name)

    def state_dict(self, destination=None, prefix=''):
        if destination is None:
            destination = OrderedDict()
        for name, param in chain(self._buffers.items(), self._parameters.items()):
            if param is not None:
                destination[prefix + name] = param
        return destination

    def load_state_dict(self, state_dict, prefix=''):
        for name, param in self._parameters.items():
            new_param = state_dict.get(prefix + name, param)
            if not isinstance(new_param, Parameter) and new_param is not None:
                raise TypeError(
                    "expected torch.autograd.Parameter for key '{}' (got {})"
                    .format(prefix + name, torch.typename(new_param)))
            self._parameters[name] = new_param
        for name, buf in self._buffers.items():
            self._buffers[name] = state_dict.get(prefix + name, buf)

    def parameters(self, memo=None):
        if memo is None:
            memo = set()
        for p in self._parameters.values():
            if p is not None and p not in memo:
                memo.add(p)
                yield p

    def children(self):
        if False:
            yield

    def modules(self, memo=None):
        if memo is None:
            memo = set()
        if self not in memo:
            memo.add(self)
            yield self

    def train(self):
        self.training = True
        return self

    def eval(self):
        self.training = False
        return self

    def zero_grad(self):
        for p in self.parameters():
            p.grad.zero_()

    def share_memory(self):
        return self._apply(lambda t: t.share_memory_())

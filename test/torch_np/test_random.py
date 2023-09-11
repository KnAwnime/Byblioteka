# Owner(s): ["module: dynamo"]

"""Light smoke test switching between numpy to pytorch random streams.
"""
from contextlib import contextmanager

import numpy as _np
import pytest

import torch._numpy as tnp
from torch._numpy.testing import assert_equal


@contextmanager
def control_stream(use_numpy=False):
    oldstate = tnp.random.USE_NUMPY_RANDOM
    tnp.random.USE_NUMPY_RANDOM = use_numpy
    try:
        yield
    finally:
        tnp.random.USE_NUMPY_RANDOM = oldstate


@pytest.mark.parametrize("use_numpy", [True, False])
@pytest.mark.parametrize(
    "name, arg",
    [
        ("normal", ()),
        ("rand", ()),
        ("randint", (0, 5)),
        ("randn", ()),
        ("random", ()),
        ("random_sample", ()),
        ("sample", ()),
        ("uniform", ()),
    ],
)
class TestScalarReturn:
    def test_scalar(self, name, arg, use_numpy):
        # default `size` means a python scalar return
        func = getattr(tnp.random, name)
        with control_stream(use_numpy):
            r = func(*arg)
        assert isinstance(r, (int, float))

    def test_array(self, name, arg, use_numpy):
        func = getattr(tnp.random, name)
        with control_stream(use_numpy):
            if name in ["rand", "randn"]:
                arg = arg + (10,)
                r = func(*arg)
            else:
                r = func(*arg, size=10)
        assert isinstance(r, tnp.ndarray)


class TestShuffle:
    @pytest.mark.parametrize("use_numpy", [True, False])
    def test_1d(self, use_numpy):
        ax = tnp.asarray([1, 2, 3, 4, 5, 6])
        ox = ax.copy()

        tnp.random.seed(1234)
        tnp.random.shuffle(ax)

        assert isinstance(ax, tnp.ndarray)
        assert not (ax == ox).all()

    @pytest.mark.parametrize("use_numpy", [True, False])
    def test_2d(self, use_numpy):
        # np.shuffle only shuffles the first axis
        ax = tnp.asarray([[1, 2, 3], [4, 5, 6]])
        ox = ax.copy()

        tnp.random.seed(1234)
        tnp.random.shuffle(ax)

        assert isinstance(ax, tnp.ndarray)
        assert not (ax == ox).all()

    @pytest.mark.parametrize("use_numpy", [True, False])
    def test_shuffle_list(self, use_numpy):
        # on eager, we refuse to shuffle lists
        # under dynamo, we always fall back to numpy
        # NB: this means that the random stream is different for
        # shuffling a list or an array when USE_NUMPY_STREAM == False
        x = [1, 2, 3]
        with pytest.raises(NotImplementedError):
            tnp.random.shuffle(x)


@pytest.mark.parametrize("use_numpy", [True, False])
def test_choice(use_numpy):
    kwds = dict(size=3, replace=False, p=[0.1, 0, 0.3, 0.6, 0])
    with control_stream(use_numpy):
        tnp.random.seed(12345)
        x = tnp.random.choice(5, **kwds)
        x_1 = tnp.random.choice(tnp.arange(5), **kwds)
        assert_equal(x, x_1)


class TestNumpyGlobal:
    def test_numpy_global(self):
        with control_stream(use_numpy=True):
            tnp.random.seed(12345)
            x = tnp.random.uniform(0, 1, size=11)

        # check that the stream is identical to numpy's
        _np.random.seed(12345)
        x_np = _np.random.uniform(0, 1, size=11)
        assert_equal(x, tnp.asarray(x_np))

        # switch to the pytorch stream, variates differ
        with control_stream(use_numpy=False):
            tnp.random.seed(12345)
            x_1 = tnp.random.uniform(0, 1, size=11)

        assert not (x_1 == x).all()

    def test_wrong_global(self):
        with control_stream("oops"):
            with pytest.raises(ValueError):
                tnp.random.rand()


if __name__ == "__main__":
    from torch._dynamo.test_case import run_tests

    run_tests()

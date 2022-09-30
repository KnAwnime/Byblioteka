# Owner(s): ["module: cuda"]

import os
import sys
import multiprocessing
import torch
import unittest
from unittest.mock import patch

# NOTE: Each of the tests in this module need to be run in a brand new process to ensure CUDA is uninitialized
# prior to test initiation.
# Before executing the desired tests, we need to disable CUDA initialization and fork_handler additions that would
# otherwise be triggered by the `torch.testing._internal.common_utils` module import
os.environ['PYTORCH_NVML_BASED_CUDA_CHK'] = '1'
from torch.testing._internal.common_utils import (parametrize, instantiate_parametrized_tests, run_tests, TestCase,
                                                  IS_WINDOWS)
# NOTE: Because `remove_device_and_dtype_suffixes` initializes CUDA context (triggered via the import of
# `torch.testing._internal.common_device_type` which imports `torch.testing._internal.common_cuda`) we need
# to bypass that method here which should be irrelevant to the parameterized tests in this module.
torch.testing._internal.common_utils.remove_device_and_dtype_suffixes = lambda x: x

TEST_CUDA = torch.cuda.is_available()
if not TEST_CUDA:
    print('CUDA not available, skipping tests', file=sys.stderr)
    TestCase = object  # type: ignore[misc, assignment] # noqa: F811


class TestExtendedCUDAIsAvail(TestCase):

    @staticmethod
    def in_bad_fork_test() -> bool:
        _ = torch.cuda.is_available()
        return torch.cuda._is_in_bad_fork()

    # These tests validate the behavior and activation of the weaker, NVML-based, user-requested
    # `torch.cuda.is_available()` assessment. The NVML-based assessment should be attempted when
    # `PYTORCH_NVML_BASED_CUDA_CHK` is set to 1, reverting to the default CUDA Runtime API check otherwise.
    # If the NVML-based assessment is attempted but fails, the CUDA Runtime API check should be executed
    @unittest.skipIf(IS_WINDOWS, "Needs fork")
    @parametrize("nvml_avail", [True, False])
    @parametrize("avoid_init", ['1', '0', None])
    def test_cuda_is_available(self, avoid_init, nvml_avail):
        if avoid_init:
            os.environ['PYTORCH_NVML_BASED_CUDA_CHK'] = avoid_init
        else:
            del os.environ['PYTORCH_NVML_BASED_CUDA_CHK']
        torch.cuda.device_count.cache_clear()  # clear the lru_cache on this method before our test
        if nvml_avail:
            _ = torch.cuda.is_available()
        else:
            with patch.object(torch.cuda, '_device_count_nvml', return_value=-1):
                _ = torch.cuda.is_available()
        with multiprocessing.get_context("fork").Pool(1) as pool:
            in_bad_fork = pool.apply(TestExtendedCUDAIsAvail.in_bad_fork_test)
        if os.getenv('PYTORCH_NVML_BASED_CUDA_CHK') == '1' and nvml_avail:
            assert not in_bad_fork
        else:
            assert in_bad_fork


instantiate_parametrized_tests(TestExtendedCUDAIsAvail)

if __name__ == '__main__':
    run_tests()

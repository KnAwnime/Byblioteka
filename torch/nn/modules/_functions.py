import torch
from torch.autograd.function import Function


class SyncBatchNorm(Function):

    @staticmethod
    def forward(self, input, weight, bias, running_mean, running_var, eps, momentum, process_group, world_size):
        input = input.contiguous()

        # calcualte mean/invstd for input.
        mean, invstd = torch.batch_norm_stats(input, eps)

        mean_all = torch.empty(world_size, mean.size(0), dtype=mean.dtype, device=mean.device)
        invstd_all = torch.empty(world_size, invstd.size(0), dtype=invstd.dtype, device=invstd.device)
        mean_l = list(mean_all.unbind(0))
        invstd_l = list(invstd_all.unbind(0))
        # using all_gather instead of all reduce so we can calculate mean/var in one go
        mean_all_reduce = torch.distributed.all_gather(mean_l, mean, process_group, async_op=True)
        invstd_all_reduce = torch.distributed.all_gather(invstd_l, invstd, process_group, async_op=True)

        # wait on the async communication to finish
        mean_all_reduce.wait()
        invstd_all_reduce.wait()

        # calcualte global mean & invstd
        mean, invstd = torch.batch_norm_gather_stats(
            input,
            mean_all,
            invstd_all,
            running_mean,
            running_var,
            momentum,
            eps,
            int(input.numel() / input.size(1))
        )

        self.save_for_backward(input, weight, mean, invstd)
        self.process_group = process_group
        self.world_size = world_size

        # apply element-wise normalization
        out = torch.batch_norm_elemt(input, weight, bias, mean, invstd, eps)
        return out

    @staticmethod
    def backward(self, grad_output):
        grad_output = grad_output.contiguous()
        saved_input, weight, mean, invstd = self.saved_tensors
        grad_input = grad_weight = grad_bias = None
        process_group = self.process_group
        world_size = self.world_size

        # calculate local stats as well as grad_weight / grad_bias
        mean_dy, mean_dy_xmu, grad_weight, grad_bias = torch.batch_norm_backward_reduce(
            grad_output,
            saved_input,
            mean,
            invstd,
            self.needs_input_grad[0],
            self.needs_input_grad[1],
            self.needs_input_grad[2]
        )

        if self.needs_input_grad[0]:
            # synchronizing stats used to calculate input gradient.
            # TODO: move div_ into batch_norm_backward_elemt kernel
            mean_dy_all_reduce = torch.distributed.all_reduce(
                mean_dy, torch.distributed.ReduceOp.SUM, process_group, async_op=True)
            mean_dy_xmu_all_reduce = torch.distributed.all_reduce(
                mean_dy_xmu, torch.distributed.ReduceOp.SUM, process_group, async_op=True)

            # wait on the async communication to finish
            mean_dy_all_reduce.wait()
            mean_dy_xmu_all_reduce.wait()

            mean_dy.div_(world_size)
            mean_dy_xmu.div_(world_size)
            # backward pass for gradient calculation
            grad_input = torch.batch_norm_backward_elemt(
                grad_output,
                saved_input,
                mean,
                invstd,
                weight,
                mean_dy,
                mean_dy_xmu
            )

        # synchronizing of grad_weight / grad_bias is not needed as distributed
        # training would handle all reduce.
        if weight is None or not self.needs_input_grad[1]:
            grad_weight = None

        if weight is None or not self.needs_input_grad[2]:
            grad_bias = None

        return grad_input, grad_weight, grad_bias, None, None, None, None, None, None

    @classmethod
    def convert_sync_batchnorm(cls, module, process_group=None):
        r"""Helper function to convert `torch.nn.BatchNormND` layer in the model to
        `torch.nn.SyncBatchNorm` layer.

        Args:
            module (nn.Module): containing module
            process_group (optional): process group to scope synchronization,
        default is the whole world

        Returns:
            The original module with the converted `torch.nn.SyncBatchNorm` layer

        Example::

            >>> # Network with nn.BatchNorm layer
            >>> module = torch.nn.Sequential(
            >>>            torch.nn.Linear(20, 100),
            >>>            torch.nn.BatchNorm1d(100)
            >>>          ).cuda()
            >>> # creating process group (optional)
            >>> # process_ids is a list of int identifying rank ids.
            >>> process_group = torch.distributed.new_group(process_ids)
            >>> sync_bn_module = convert_sync_batchnorm(module, process_group)

        """
        module_output = module
        if isinstance(module, torch.nn.modules.batchnorm._BatchNorm):
            module_output = torch.nn.SyncBatchNorm(module.num_features,
                                                   module.eps, module.momentum,
                                                   module.affine,
                                                   module.track_running_stats,
                                                   process_group)
            if module.affine:
                module_output.weight.data = module.weight.data.clone().detach()
                module_output.bias.data = module.bias.data.clone().detach()
            module_output.running_mean = module.running_mean
            module_output.running_var = module.running_var
            module_output.num_batches_tracked = module.num_batches_tracked
        for name, child in module.named_children():
            module_output.add_module(name, self.convert_sync_batchnorm(child))
        del module
        return module_output

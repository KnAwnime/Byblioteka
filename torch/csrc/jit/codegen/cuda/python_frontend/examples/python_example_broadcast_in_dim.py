import torch

from torch._C._nvfuser import Fusion, FusionDefinition

# Construct and Define Fusion
fusion = Fusion()

with FusionDefinition(fusion) as fd :
    t0 = fd.define_tensor(1)
    t1 = fd.define_tensor(3)

    fd.add_input(t0)
    fd.add_input(t1)

    t0_b = fd.Ops.broadcast_in_dim(t0, [2, 3, 4], [1])
    t2 = fd.Ops.add(t0_b, t1)

    fd.add_output(t2)

fusion.print_ir()

# Execute Fusion
input1 = torch.ones(3, device='cuda')
input2 = torch.ones(2, 3, 4, device='cuda')

# Kernel compilation should be cached for the 2nd iteration
# with input tensors of the same shape
for _ in range(5) :
    outputs = fusion.execute([input1, input2])

print(outputs[0])

from collections import defaultdict

from torch.utils.data import IterDataPipe, functional_datapipe, DataChunk
from torch.utils.data.datapipes.utils.common import check_lambda_fn
from typing import Any, Callable, DefaultDict, Iterator, List, Optional, Sized, TypeVar

T_co = TypeVar('T_co', covariant=True)


@functional_datapipe('sharding_filter')
class ShardingFilterIterDataPipe(IterDataPipe):
    r"""
    Wrapper that allows DataPipe to be sharded (functional name: ``sharding_filter``). After ``apply_sharding`` is
    called, each instance of the DataPipe (on different workers) will have every `n`-th element of the
    original DataPipe, where `n` equals to the number of instances.

    Args:
        source_datapipe: Iterable DataPipe that will be sharded
    """
    def __init__(self, source_datapipe: IterDataPipe):
        self.source_datapipe = source_datapipe
        self.num_of_instances = 1
        self.instance_id = 0

    def is_shardable(self):
        return True

    def apply_sharding(self, num_of_instances, instance_id):
        self.num_of_instances = num_of_instances
        self.instance_id = instance_id

    def __iter__(self):
        for i, item in enumerate(self.source_datapipe):
            if i % self.num_of_instances == self.instance_id:
                yield item

    def __len__(self):
        if isinstance(self.source_datapipe, Sized):
            return len(self.source_datapipe) // self.num_of_instances +\
                (1 if (self.instance_id < len(self.source_datapipe) % self.num_of_instances) else 0)
        raise TypeError("{} instance doesn't have valid length".format(type(self).__name__))


@functional_datapipe('batch')
class BatcherIterDataPipe(IterDataPipe[DataChunk]):
    r"""
    Creates mini-batches of data (functional name: ``batch``). An outer dimension will be added as
    ``batch_size`` if ``drop_last`` is set to ``True``, or ``length % batch_size`` for the
    last batch if ``drop_last`` is set to ``False``.

    Args:
        datapipe: Iterable DataPipe being batched
        batch_size: The size of each batch
        drop_last: Option to drop the last batch if it's not full
        wrapper_class: wrapper to apply onto each batch (type ``List``) before yielding,
            defaults to ``DataChunk``
    """
    datapipe: IterDataPipe
    batch_size: int
    drop_last: bool
    length: Optional[int]

    def __init__(self,
                 datapipe: IterDataPipe,
                 batch_size: int,
                 drop_last: bool = False,
                 wrapper_class=DataChunk,
                 ) -> None:
        assert batch_size > 0, "Batch size is required to be larger than 0!"
        super().__init__()
        self.datapipe = datapipe
        self.batch_size = batch_size
        self.drop_last = drop_last
        self.length = None
        self.wrapper_class = wrapper_class

    def __iter__(self) -> Iterator[DataChunk]:
        batch: List = []
        for x in self.datapipe:
            batch.append(x)
            if len(batch) == self.batch_size:
                yield self.wrapper_class(batch)
                batch = []
        if len(batch) > 0:
            if not self.drop_last:
                yield self.wrapper_class(batch)

    def __len__(self) -> int:
        if self.length is not None:
            return self.length
        if isinstance(self.datapipe, Sized):
            if self.drop_last:
                self.length = len(self.datapipe) // self.batch_size
            else:
                self.length = (len(self.datapipe) + self.batch_size - 1) // self.batch_size
            return self.length
        raise TypeError("{} instance doesn't have valid length".format(type(self).__name__))


@functional_datapipe('unbatch')
class UnBatcherIterDataPipe(IterDataPipe):
    r"""
    Undoes batching of data (functional name: ``unbatch``). In other words, it flattens the data up to the specified level
    within a batched DataPipe.

    Args:
        datapipe: Iterable DataPipe being un-batched
        unbatch_level: Defaults to ``1`` (only flattening the top level). If set to ``2``,
            it will flatten the top two levels, and ``-1`` will flatten the entire DataPipe.
    """

    def __init__(self,
                 datapipe: IterDataPipe,
                 unbatch_level: int = 1):
        self.datapipe = datapipe
        self.unbatch_level = unbatch_level

    def __iter__(self):
        for element in self.datapipe:
            for i in self._dive(element, unbatch_level=self.unbatch_level):
                yield i

    def _dive(self, element, unbatch_level):
        if unbatch_level < -1:
            raise ValueError("unbatch_level must be -1 or >= 0")
        if unbatch_level == -1:
            if isinstance(element, list) or isinstance(element, DataChunk):
                for item in element:
                    for i in self._dive(item, unbatch_level=-1):
                        yield i
            else:
                yield element
        elif unbatch_level == 0:
            yield element
        else:
            if isinstance(element, list) or isinstance(element, DataChunk):
                for item in element:
                    for i in self._dive(item, unbatch_level=unbatch_level - 1):
                        yield i
            else:
                raise IndexError(f"unbatch_level {self.unbatch_level} exceeds the depth of the DataPipe")


@functional_datapipe('groupby')
class GrouperIterDataPipe(IterDataPipe[DataChunk]):
    r"""
    Groups data from input IterDataPipe by keys which are generated from ``group_key_fn``,
    and yields a ``DataChunk`` with size ranging from ``guaranteed_group_size``
    to ``group_size`` (functional name: ``groupby``).

    Args:
        datapipe: Iterable datapipe to be grouped
        group_key_fn: Function used to generate group key from the data of the source datapipe
        buffer_size: The size of buffer for ungrouped data
        group_size: The size of each group
        guaranteed_group_size: The guaranteed minimum group size
        drop_remaining: Specifies if the group smaller than `guaranteed_group_size` will be dropped from buffer
    """
    def __init__(self,
                 datapipe: IterDataPipe[T_co],
                 group_key_fn: Callable,
                 *,
                 buffer_size: int = 10000,
                 group_size: Optional[int] = None,
                 guaranteed_group_size: Optional[int] = None,
                 drop_remaining: bool = False):
        check_lambda_fn(group_key_fn)
        self.datapipe = datapipe
        self.group_key_fn = group_key_fn

        self.buffer_size = buffer_size
        self.group_size = group_size
        self.guaranteed_group_size = None
        if group_size is not None and buffer_size is not None:
            assert 0 < group_size <= buffer_size
            self.guaranteed_group_size = group_size
        if guaranteed_group_size is not None:
            assert group_size is not None and 0 < guaranteed_group_size <= group_size
            self.guaranteed_group_size = guaranteed_group_size
        self.drop_remaining = drop_remaining
        self.wrapper_class = DataChunk

    def _remove_biggest_key(self, buffer_elements, buffer_size):
        biggest_key = None
        biggest_size = 0
        result_to_yield = None
        for findkey in buffer_elements.keys():
            if len(buffer_elements[findkey]) > biggest_size:
                biggest_size = len(buffer_elements[findkey])
                biggest_key = findkey

        if self.guaranteed_group_size is not None and biggest_size < self.guaranteed_group_size and not self.drop_remaining:
            raise RuntimeError('Failed to group items', str(buffer_elements[biggest_key]))

        if self.guaranteed_group_size is None or biggest_size >= self.guaranteed_group_size:
            result_to_yield = buffer_elements[biggest_key]

        new_buffer_size = buffer_size - biggest_size
        del buffer_elements[biggest_key]

        return result_to_yield, new_buffer_size

    def __iter__(self):
        buffer_elements: DefaultDict[Any, List] = defaultdict(list)
        buffer_size = 0
        for x in self.datapipe:
            key = self.group_key_fn(x)

            buffer_elements[key].append(x)
            buffer_size += 1

            if self.group_size is not None and self.group_size == len(buffer_elements[key]):
                yield self.wrapper_class(buffer_elements[key])
                buffer_size -= len(buffer_elements[key])
                del buffer_elements[key]

            if buffer_size == self.buffer_size:
                (result_to_yield, buffer_size) = self._remove_biggest_key(buffer_elements, buffer_size)
                if result_to_yield is not None:
                    yield self.wrapper_class(result_to_yield)

        for key in tuple(buffer_elements.keys()):
            res = buffer_elements.pop(key)
            buffer_size -= len(res)
            yield self.wrapper_class(res)

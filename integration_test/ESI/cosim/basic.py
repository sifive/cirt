#!/usr/bin/python3

import random
import cosim


class BasicSystemTester(cosim.CosimBase):
    """Provides methods to test the 'basic' simulation."""

    def testIntAcc(self, num_msgs):
        ep = self.openEP(1, sendType=self.schema.I32,
                         recvType=self.schema.I32)
        sum = 0
        for _ in range(num_msgs):
            i = random.randint(0, 77)
            sum += i
            print(f"Sending {i}")
            ep.send(self.schema.I32.new_message(i=i))
            result = self.readMsg(ep, self.schema.I32)
            print(f"Got {result}")
            assert (result.i == sum)

    def testVectorSum(self, num_msgs):
        ep = self.openEP(2, sendType=self.schema.I32,
                         recvType=self.schema.ArrayOfI16)
        for _ in range(num_msgs):
            arr = [random.randint(0, 77), random.randint(0, 77)]
            print(f"Sending {arr}")
            ep.send(self.schema.ArrayOfI16.new_message(i=arr))
            result = self.readMsg(ep, self.schema.I32)
            print(f"Got {result}")
            assert (result.i == arr[0] + arr[1])

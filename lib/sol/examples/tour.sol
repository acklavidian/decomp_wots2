#include "lib.h"
// A tour of the language: solrun examples/tour.sol
Require("helper");
print(GREETING, " from Sol");
Fact = def(n) {
    if (n < 2) { return 1; }
    return n * Fact(n - 1);
};
print("5! = ", Fact(5));
print("twice 21 = ", Twice(21));
print("no precedence: 2 * 3 + 4 = ", 2 * 3 + 4, ", 10 - 2 - 3 = ", 10 - 2 - 3);
var total = 0;
var limit = LIMIT;
for (var i = 0; i < limit; i += 1) { total += i; }
print("sum 0..4 = ", total);
var k = 0;
while (k < 3) { k += 1; if (k == 2) { print("k is two"); } else { print("k = ", k); } }
switch (k) case 1 { print("one"); } case 3 { print("three"); } else { print("other"); }
L = [1, 2.5, "three"];
print("list size ", L.size(), ", L[2] = ", L[2]);
Point = new object;
Point.x = 1.5;
Point.Len = def { return this.x * 2.0; };
print("method: ", Point.Len());
StartThread(root, def { Wait(1.0); print("thread after 1 s"); }, []);
Wait(2.0);
print("done, random ", Rand(100));

#define REFTERM_VERSION 1

#if _DEBUG
#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#else
#define Assert(cond)
#endif

#define AssertHR(hr) Assert(SUCCEEDED(hr))
#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))

#define IsPowerOfTwo(Value) (((Value) & ((Value) - 1)) == 0)
#define SafeRatio1(A, B) ((B) ? ((A)/(B)) : (A))

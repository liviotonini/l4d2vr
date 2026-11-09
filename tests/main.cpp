#include "test_common.h"

void RunHandTests();

int main()
{
	RunHandTests();

	printf("\n%d checks, %d failed\n", g_TestChecks, g_TestFailures);
	return g_TestFailures == 0 ? 0 : 1;
}

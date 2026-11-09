#include "test_common.h"

void RunHandTests();
void RunWristHudTests();
void RunPoseTests();

int main()
{
	RunHandTests();
	RunWristHudTests();
	RunPoseTests();

	printf("\n%d checks, %d failed\n", g_TestChecks, g_TestFailures);
	return g_TestFailures == 0 ? 0 : 1;
}

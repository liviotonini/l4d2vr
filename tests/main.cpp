#include "test_common.h"

void RunHandTests();
void RunWristHudTests();
void RunPoseTests();
void RunReloadTests();

int main()
{
	RunHandTests();
	RunWristHudTests();
	RunPoseTests();
	RunReloadTests();

	printf("\n%d checks, %d failed\n", g_TestChecks, g_TestFailures);
	return g_TestFailures == 0 ? 0 : 1;
}

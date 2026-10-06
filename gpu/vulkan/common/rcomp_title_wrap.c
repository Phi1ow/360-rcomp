/*
 * R-comp - title wrapper for the PS5 test programs linked with PS5_Vulkan
 * (probe, draw test, M5). SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The program's own main() is compiled as rcomp_program_main (-Dmain=...).
 * This main() sends stdout/stderr to the title log that
 * platform/ps5/tools/run_title.sh downloads and frames the run the same way
 * as the platform title:
 *   RCOMP-TITLE begin title=<ID> program=<name> log=<path>
 *   ... program output ...
 *   RCOMP-TITLE end status=<exit status of the program>
 * then parks (see the end of main()). Absolute paths only (no cwd on the console); /app0 as fallback.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef RCOMP_TITLE_ID
#define RCOMP_TITLE_ID "PPSA88360"
#endif
#ifndef RCOMP_PROGRAM_NAME
#define RCOMP_PROGRAM_NAME "unknown"
#endif

int rcomp_program_main(int argc, char **argv);
int sceKernelUsleep(unsigned int usec);

int
main(int argc, char **argv)
{
   static const char *const paths[] = {"/data/homebrew/" RCOMP_TITLE_ID "/rcomp_title.log",
                                       "/app0/rcomp_title.log"};
   const char *used = "stdout";
   for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
      FILE *f = fopen(paths[i], "a");
      if (f) {
         fclose(f);
         if (freopen(paths[i], "a", stdout)) {
            used = paths[i];
            (void)!freopen(paths[i], "a", stderr);
            break;
         }
      }
   }
   setvbuf(stdout, NULL, _IOLBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);
   printf("RCOMP-TITLE begin title=%s program=%s pid=%d log=%s\n", RCOMP_TITLE_ID, RCOMP_PROGRAM_NAME,
          (int)getpid(), used);
#ifdef RCOMP_VK_PROFILE
   /* A title has no environment of its own. With the adapted fork link the
    * driver reads PS5VK_EXPERIMENTAL through rcomp_vk_getenv
    * (gpu/vulkan/ps5/ps5vk_title_shims.c); fix the profile before the program
    * creates its Vulkan instance. */
   {
      extern void rcomp_vk_set_profile(int experimental);
      rcomp_vk_set_profile(RCOMP_VK_PROFILE);
      printf("RCOMP-TITLE vk_profile=%s\n", RCOMP_VK_PROFILE ? "experimental" : "default");
   }
#endif
   fflush(stdout);
   int status = rcomp_program_main(argc, argv);
   fflush(stderr);
   printf("RCOMP-TITLE end status=%d\n", status);
   fflush(stdout);
   /* Never return: on the console, libc exit() makes the kernel raise SIGSYS
    * and leaves the process suspended (PS5 run of kit f414844,
    * docs/PS5_RESULTS.md O1), and an application cannot close itself
    * (sceLncUtilKillApp on itself: 0x8094000f). The log is complete; park
    * until the harness closes the title (ps5vkctl "kill <ID>"). */
   for (;;)
      sceKernelUsleep(1000000);
}

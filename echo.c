#include <syscall.h>   /* syscall prototypes */
#include <string.h>    /* strlen */
#include <stdbool.h>   /* bool, true/false */
#include <stdlib.h> // new -> EXIT_SUCCESS

int
main (int argc, char **argv)
{
  /* 파일 디스크립터 1은 STDOUT(표준 출력) */
  const int fd = 1;

  for (int i = 1; i < argc; i++)
    {
      const char *s = argv[i];
      size_t len = strlen (s);
      /* 인자 문자열 출력 */
      write (fd, s, len);
      /* 마지막 인자가 아니면 공백 한 칸 출력 */
      if (i + 1 < argc)
        write (fd, " ", 1);
    }

  /* 줄 바꿈 */
  write (fd, "\n", 1);

  return EXIT_SUCCESS;
}
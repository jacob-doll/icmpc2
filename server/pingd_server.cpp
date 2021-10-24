#include <map>
#include <iostream>
#include <cstdio>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char **argv)
{

  int ret;
  umask(0);
  if ((ret = mkfifo("/tmp/pingd", S_IFIFO | 0666)) == -1) {
    if (errno != EEXIST) {
      perror("mkfifo");
      exit(-1);
    }
  }

  while (1) {
    int fd = open("/tmp/pingd", O_RDONLY);
    char buf[1024];
    size_t nbytes = read(fd, buf, sizeof(buf));
    std::string cmd{ buf, nbytes };
    std::cout << "Read: " << cmd << "\n";
    close(fd);
  }

  return 0;
}
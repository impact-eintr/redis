#include <stdio.h>

int main(int argc, char **argv){
  int a[3][4];

  for (int i = 0;i < 3;i++) {
    for (int j = 0;j < 4;j++) {
      a[i][j] = i+j;
    }
  }
  printf("%p\n", a[1]+1);
  printf("%p\n", &a[1][1]);
  printf("%p\n", (*(a+1))+1);
  printf("%p %p\n",a, a+5);
}

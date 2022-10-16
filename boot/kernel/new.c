#include "stdio.h"


int main() {
  int a[10], b[10], i, n, c[10];
  printf("Enter the length of array");
  scanf("%d", &n);
  printf("Enter the array - a elements");
  for(i=0;i<n;i++) {
    scanf("%d", &a[i]);
  }
  printf("Enter the array - b elements");
  for(i=0;i<n;i++) {
    scanf("%d", &b[i]);
  }
  
  for(i=0;i<n;i++) {
    c[i] = a[i] + b[i];
  }
  printf("The added array is :");
  for(i=0;i<n;i++) {
    printf("%d", c[i]);
  } return 0;
  
}

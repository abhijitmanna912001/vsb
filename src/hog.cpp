// Burns CPU forever. Used to show scheduler behaviour under load.
int main() {
  volatile unsigned long x = 0;
  for (;;) {
    x++;
  }
  return 0;
}

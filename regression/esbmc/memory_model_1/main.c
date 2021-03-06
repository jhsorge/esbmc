#include <stdlib.h>
#include <assert.h>

_Bool nondet_bool();

typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

struct foo
{
  uint16_t bar[2];
  uint8_t baz;
};

int main()
{
  struct foo *quux = (struct foo *)malloc(sizeof(struct foo));
  if(quux == NULL)
    return -1;
  quux->bar[0] = 1;
  quux->bar[1] = 2;
  quux->baz = 'c';
  uint16_t *fuzz;
  if(nondet_bool())
  {
    fuzz = &quux->bar[0];
  }
  else
  {
    fuzz = &quux->baz;
  }
  assert(*fuzz == 1 || *fuzz == 2 || *fuzz == 'c');
}

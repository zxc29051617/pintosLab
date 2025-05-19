#include "vm/vm.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/thread.h"

void 
vm_init (void)
{

  vm_frame_init ();
  vm_swap_init ();
} 
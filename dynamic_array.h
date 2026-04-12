/*
 * This file contains only a number of macro, struct definition(s) and maybe
 * some inline functions to manipulate caller's allocated chunk of memory in the
 * form of an array. The array is expected to follow the model of storing
 * metadata at the beginning of the array. The metadata is expected to be of the
 * type `struct arr_meta` defined herein.
 *
 * You may extend the functionalities in this file to implement more complex
 * functionalities for your array. But please make sure that your array at its
 * foundational level follow the model mentioned above.
 */

#include <stdlib.h>

struct arr_meta {
  /**
   * Actual number of elements in a given array.
   */
  size_t length;
  /**
   * Accounts for the number of elements a given array can hold,
   * it doesn't account for the metadata.
   */
  size_t capacity;
};

// Please mark sure not to set these less than 2.
// Unless capacity incrementation factor is from 2 and above.
#define ARR_INIT_CAPACITY 2
#define ARR_INCREMENT_FACTOR 1.5
#define ARR_HEADER(a) (a ? ((struct arr_meta *)(a)) - 1 : NULL)
#define ARR_LENGTH(a) (a ? ARR_HEADER(a)->length : 0)
#define ARR_CAPACITY(a) (a ? ARR_HEADER(a)->capacity : 0)
#define ARR_NEW_CAPACITY(a)                                                    \
  (a ? ((size_t)(ARR_CAPACITY(a) * ARR_INCREMENT_FACTOR)) : ARR_INIT_CAPACITY)
#define ARR_FREE(a) (a ? free(ARR_HEADER(a)) : free(NULL))
#define ARR_SET_LENGTH(a, n) (ARR_HEADER(a)->length = (n))
#define ARR_SET_CAPACITY(a, n) (ARR_HEADER(a)->capacity = (n))
#define ARR_RESIZE(a, new_capacity)                                            \
  (realloc(ARR_HEADER(a), new_capacity * sizeof(*a) + sizeof(struct arr_meta)))

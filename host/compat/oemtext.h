#ifndef NP2_HOST_OEMTEXT_H
#define NP2_HOST_OEMTEXT_H

#include <codecnv/codecnv.h>

#define oemtext_oemtosjis(output, output_count, input, input_count) \
    codecnv_utf8tosjis((output), (output_count), (input), (input_count))
#define oemtext_sjistooem(output, output_count, input, input_count) \
    codecnv_sjistoutf8((output), (output_count), (input), (input_count))

#endif /* NP2_HOST_OEMTEXT_H */

#ifndef __LOGJAM_IMPORTER_INDEXER_H_INCLUDED__
#define __LOGJAM_IMPORTER_INDEXER_H_INCLUDED__

#include "importer-common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void indexer(zsock_t *pipe, void *args);

#ifdef __cplusplus
}
#endif

#endif

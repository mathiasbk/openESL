/* A model registry for the same_as tests (#181), in place of shared/models_gen.h: the test
 * build force-includes it (-include), so the guard below keeps the real one out. Model 3 is a
 * 2.9" screen that uses the file of 2.9; model 4 names 1.54 but has another screen, which the
 * AP must refuse (tools/gen_models.py refuses it in shared/models.json). Models 5 and 6 have
 * yellow (black/white/red/yellow and black/white/yellow), for test_yellow.c: no model in
 * shared/models.json has it yet. */
#ifndef ESL_MODELS_GEN_H
#define ESL_MODELS_GEN_H

#define ESL_MODEL_29       1
#define ESL_MODEL_154      2
#define ESL_MODEL_29B      3
#define ESL_MODEL_BAD      4
#define ESL_MODEL_BWRY     5
#define ESL_MODEL_BWY      6
#define ESL_MODEL_COUNT    7

#define ESL_MODEL_29_W 296
#define ESL_MODEL_29_H 128
#define ESL_MODEL_154_W 200
#define ESL_MODEL_154_H 200

#define ESL_MODEL_LIST(X) \
    X(1, "2.9", "solum-m2-29-bw", 296, 128, 0, 0) \
    X(2, "1.54", "solum-m2-154-bw", 200, 200, 0, 0) \
    X(3, "2.9b", "other-29-bw", 296, 128, 0, 0) \
    X(4, "bad", "other-bad", 296, 128, 0, 0) \
    X(5, "2.9-bwry", "other-29-bwry", 296, 128, 1, 1) \
    X(6, "2.9-bwy", "other-29-bwy", 296, 128, 0, 1) \

#define ESL_MODEL_SAME_AS_LIST(X) \
    X(3, 1) \
    X(4, 2) \

#endif

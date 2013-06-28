/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// To see assembly:
//
//   g++ -fpermissive -S -O4 -o test.s -I/usr/local/src/jdk1.6.0_32/include -I/usr/local/src/jdk1.6.0_32/include/linux /l/nativebq/lucene/misc/src/java/org/apache/lucene/search/NativeSearch.cpp
//

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <jni.h>
#include <math.h> // for sqrt
#include <stdlib.h> // malloc
#include <string.h> // memcpy

#include "common.h"

//static FILE *fp = fopen("logout.txt", "w");

// Returns 1 if the bit was not previously set
inline static void setLongBit(long *bits, int index) {
  int wordNum = index >> 6;      // div 64
  int bit = index & 0x3f;     // mod 64
  long bitmask = 1L << bit;
  bits[wordNum] |= bitmask;
}

static int getLongBit(long *bits, int index) {
  int wordNum = index >> 6;      // div 64
  int bit = index & 0x3f;     // mod 64
  long bitmask = 1L << bit;
  return (bits[wordNum] & bitmask) != 0;
}

static bool initSub(int id,
                    PostingsState *sub,
                    bool docsOnly,
                    int singletonDocID,
                    long totalTermFreq,
                    int docFreq,
                    bool skipFreqs,
                    long docFileAddress,
                    long docTermStartFP
                    ) {
  sub->docsOnly = docsOnly;
  sub->id = id;
  if (singletonDocID != -1) {
    sub->nextDocID = singletonDocID;
    sub->docsLeft = 0;
    sub->docFreqBlockLastRead = 0;
    sub->docFreqBlockEnd = 0;
    sub->docDeltas = 0;
    sub->freqs = (unsigned int *) malloc(sizeof(int));
    if (sub->freqs == 0) {
      return false;
    }
    sub->freqs[0] = (int) totalTermFreq;
  } else {
    sub->docsLeft = docFreq;
    if (!skipFreqs) {
      sub->docDeltas = (unsigned int *) malloc(2*BLOCK_SIZE*sizeof(int));
      if (sub->docDeltas == 0) {
        return false;
      }
      // Locality seemed to help here:
      sub->freqs = sub->docDeltas + BLOCK_SIZE;
    } else {
      sub->docDeltas = (unsigned int *) malloc(BLOCK_SIZE*sizeof(int));
      if (sub->docDeltas == 0) {
        return false;
      }

      sub->freqs = 0;
    }
    //printf("docFileAddress=%ld startFP=%ld scores=%lx\n", docFileAddress, docTermStartFPs[i], scores);fflush(stdout);
    sub->docFreqs = ((unsigned char *) docFileAddress) + docTermStartFP;
    nextDocFreqBlock(sub);
    sub->nextDocID = sub->docDeltas[0];
    sub->docFreqBlockLastRead = 0;
  }

  return true;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_apache_lucene_search_NativeSearch_searchSegmentBooleanQuery
  (JNIEnv *env,
   jclass cl,

   // PQ holding top hits so far, pre-filled with sentinel
   // values: 
   jintArray jtopDocIDs,
   jfloatArray jtopScores,

   // Current segment's maxDoc
   jint maxDoc,

   // Current segment's docBase
   jint docBase,

   // Current segment's liveDocs, or null:
   jbyteArray jliveDocsBytes,

   // weightValue from each TermWeight:
   jfloatArray jtermWeights,

   // Norms for the field (all TermQuery must be against a single field):
   jbyteArray jnorms,

   // nocommit silly to pass this once for each segment:
   // Cache, mapping byte norm -> float
   jfloatArray jnormTable,

   // Coord factors from BQ:
   jfloatArray jcoordFactors,

   // If the term has only one docID in this segment (it was
   // "pulsed") then its set here, else -1:
   jintArray jsingletonDocIDs,

   jlongArray jtotalTermFreqs,

   // docFreq of each term
   jintArray jdocFreqs,

   // Offset in the .doc file where this term's docs+freqs begin:
   jlongArray jdocTermStartFPs,

   // Address in memory where .doc file is mapped:
   jlong docFileAddress,

   // First numNot clauses are MUST_NOT
   jint numMustNot,

   // Next numMust clauses are MUST
   jint numMust,

   jint dsNumDims,

   jintArray jdsTotalHits,

   jintArray jdsTermsPerDim,

   jlongArray jdsHitBits,

   jobjectArray jdsNearMissBits,

   jintArray jdsSingletonDocIDs,

   jintArray jdsDocFreqs,

   jlongArray jdsDocTermStartFPs,

   jlong dsDocFileAddress)
{

  // Clauses come in sorted MUST_NOT (docFreq descending),
  // MUST (docFreq ascending), SHOULD (docFreq descending)

  //printf("START search\n"); fflush(stdout);

  float *scores = 0;
  int *docIDs = 0;
  unsigned int *coords = 0;
  bool failed = false;
  unsigned char *skips = 0;
  PostingsState *subs = 0;
  int *singletonDocIDs = 0;
  long *totalTermFreqs = 0;
  long *docTermStartFPs = 0;
  int *docFreqs = 0;
  float *termWeights = 0;
  float *coordFactors = 0;
  unsigned char *liveDocsBytes = 0;
  unsigned char* norms = 0;
  float *normTable = 0;
  int *topDocIDs = 0;
  float *topScores = 0;
  unsigned int *filled = 0;
  double **termScoreCache = 0;
  unsigned char isCopy = 0;
  int numScorers;
  int topN;
  PostingsState *dsSubs = 0;
  unsigned int *dsCounts = 0;
  unsigned int *dsMissingDims = 0;
  unsigned int *dsTermsPerDim = 0;
  unsigned long *dsHitBits = 0;
  unsigned long **dsNearMissBits = 0;
  unsigned long *dsDocTermStartFPs = 0;
  unsigned int *dsDocFreqs = 0;
  int *dsSingletonDocIDs = 0;
  unsigned int *dsTotalHits = 0;

  if (dsNumDims > 0) {
    dsCounts = (unsigned int *) malloc(CHUNK * sizeof(int));
    if (dsCounts == 0) {
      failed = true;
      goto end;
    }
    // Must be 1-filled:
    for(int i=0;i<CHUNK;i++) {
      dsCounts[i] = 1;
    }
    // Must be zero-filled:
    dsMissingDims = (unsigned int *) calloc(CHUNK, sizeof(int));
    if (dsMissingDims == 0) {
      failed = true;
      goto end;
    }
    dsTermsPerDim = (unsigned int *) env->GetIntArrayElements(jdsTermsPerDim, 0);
    if (dsTermsPerDim == 0) {
      failed = true;
      goto end;
    }
    int dsNumTerms = 0;
    for(int i=0;i<dsNumDims;i++) {
      dsNumTerms += dsTermsPerDim[i];
    }
    dsTotalHits = (unsigned int *) env->GetIntArrayElements(jdsTotalHits, 0);
    if (dsTotalHits == 0) {
      failed = true;
      goto end;
    }
    dsHitBits = (unsigned long *) env->GetPrimitiveArrayCritical(jdsHitBits, 0);
    if (dsHitBits == 0) {
      failed = true;
      goto end;
    }
    dsNearMissBits = (unsigned long **) calloc(dsNumDims, sizeof(long *));
    if (dsNearMissBits == 0) {
      failed = true;
      goto end;
    }
    for(int i=0;i<dsNumDims;i++) {
      jlongArray jbits = (jlongArray) env->GetObjectArrayElement(jdsNearMissBits, i);
      dsNearMissBits[i] = (unsigned long *) env->GetPrimitiveArrayCritical(jbits, 0);
      if (dsNearMissBits[i] == 0) {
        failed = true;
        goto end;
      }
    }

    dsDocFreqs = (unsigned int *) env->GetIntArrayElements(jdsDocFreqs, 0);
    if (dsDocFreqs == 0) {
      failed = true;
      goto end;
    }

    dsDocTermStartFPs = (unsigned long *) env->GetLongArrayElements(jdsDocTermStartFPs, 0);
    if (dsDocTermStartFPs == 0) {
      failed = true;
      goto end;
    }

    dsSingletonDocIDs = (int *) env->GetIntArrayElements(jdsSingletonDocIDs, 0);
    if (dsSingletonDocIDs == 0) {
      failed = true;
      goto end;
    }

    dsSubs = (PostingsState *) calloc(dsNumTerms, sizeof(PostingsState));
    if (dsSubs == 0) {
      failed = true;
      goto end;
    }

    for(int i=0;i<dsNumTerms;i++) {
      //printf("ds init sub %d: dF=%d start=%ld\n", i, dsDocFreqs[i], dsDocTermStartFPs[i]);
      if (!initSub(i, dsSubs+i, true, dsSingletonDocIDs[i], 1, dsDocFreqs[i], true, dsDocFileAddress, dsDocTermStartFPs[i])) {
        failed = true;
        goto end;
      }
    }
  }

  if (jtopScores == 0) {
    scores = 0;
  } else {
    scores = (float *) malloc(CHUNK * sizeof(float));
    if (scores == 0) {
      failed = true;
      goto end;
    }
  }
  coords = (unsigned int *) malloc(CHUNK * sizeof(int));
  if (coords == 0) {
    failed = true;
    goto end;
  }

  // Set to 1 by a MUST_NOT match:
  skips = (unsigned char *) calloc(CHUNK, sizeof(char));
  if (skips == 0) {
    failed = true;
    goto end;
  }

  docIDs = (int *) malloc(CHUNK * sizeof(int));
  if (docIDs == 0) {
    failed = true;
    goto end;
  }
  for(int i=0;i<CHUNK;i++) {
    docIDs[i] = -1;
  }

  numScorers = env->GetArrayLength(jdocFreqs);

  topN = env->GetArrayLength(jtopDocIDs) - 1;
  //printf("topN=%d\n", topN);

  subs = (PostingsState *) calloc(numScorers, sizeof(PostingsState));
  if (subs == 0) {
    failed = true;
    goto end;
  }
  singletonDocIDs = env->GetIntArrayElements(jsingletonDocIDs, 0);
  if (singletonDocIDs == 0) {
    failed = true;
    goto end;
  }
  totalTermFreqs = env->GetLongArrayElements(jtotalTermFreqs, 0);
  if (totalTermFreqs == 0) {
    failed = true;
    goto end;
  }
  docTermStartFPs = env->GetLongArrayElements(jdocTermStartFPs, 0);
  if (docTermStartFPs == 0) {
    failed = true;
    goto end;
  }
  docFreqs = env->GetIntArrayElements(jdocFreqs, 0);
  if (docFreqs == 0) {
    failed = true;
    goto end;
  }
  termWeights = (float *) env->GetFloatArrayElements(jtermWeights, 0);
  if (termWeights == 0) {
    failed = true;
    goto end;
  }
  coordFactors = (float *) env->GetFloatArrayElements(jcoordFactors, 0);
  if (coordFactors == 0) {
    failed = true;
    goto end;
  }

  liveDocsBytes;
  if (jliveDocsBytes == 0) {
    liveDocsBytes = 0;
  } else {
    liveDocsBytes = (unsigned char *) env->GetPrimitiveArrayCritical(jliveDocsBytes, &isCopy);
    if (liveDocsBytes == 0) {
      failed = true;
      goto end;
    }
    //printf("liveDocs isCopy=%d\n", isCopy);fflush(stdout);
  }

  isCopy = 0;
  norms = (unsigned char *) env->GetPrimitiveArrayCritical(jnorms, &isCopy);
  if (norms == 0) {
    failed = true;
    goto end;
  }

  isCopy = 0;
  normTable = (float *) env->GetPrimitiveArrayCritical(jnormTable, &isCopy);
  if (normTable == 0) {
    failed = true;
    goto end;
  }

  for(int i=0;i<numScorers;i++) {
    if (!initSub(i, subs+i, false, singletonDocIDs[i], totalTermFreqs[i], docFreqs[i],
                 scores == 0 || i < numMustNot, docFileAddress, docTermStartFPs[i])) {
      failed = true;
      goto end;
    }
  }

  // PQ holding top hits:
  topDocIDs = (int *) env->GetIntArrayElements(jtopDocIDs, 0);
  if (topDocIDs == 0) {
    failed = true;
    goto end;
  }

  if (jtopScores != 0) {
    topScores = (float *) env->GetFloatArrayElements(jtopScores, 0);
    if (topScores == 0) {
      failed = true;
      goto end;
    }
  } else {
    topScores = 0;
  }

  filled = (unsigned int *) malloc(CHUNK * sizeof(int));
  if (filled == 0) {
    failed = true;
    goto end;
  }

  termScoreCache = (double **) calloc(numScorers, sizeof(double*));
  if (termScoreCache == 0) {
    failed = true;
    goto end;
  }
  for(int i=0;i<numScorers;i++) {
    termScoreCache[i] = (double *) malloc(TERM_SCORES_CACHE_SIZE*sizeof(double));
    if (termScoreCache[i] == 0) {
      failed = true;
      goto end;
    }
    for(int j=0;j<TERM_SCORES_CACHE_SIZE;j++) {
      termScoreCache[i][j] = termWeights[i] * sqrt(j);
    }
  }

  int hitCount;

  if (numMustNot == 0 && numMust == 0) {
    // Only SHOULD
    hitCount = booleanQueryOnlyShould(subs, liveDocsBytes, termScoreCache, termWeights,
                                      maxDoc, topN, numScorers, docBase, filled, docIDs, scores, coords,
                                      topScores, topDocIDs, coordFactors, normTable,
                                      norms, dsSubs, dsCounts, dsMissingDims, dsNumDims, dsTotalHits, dsTermsPerDim, dsHitBits, dsNearMissBits);
  } else if (numMust == 0) {
    // At least one MUST_NOT and at least one SHOULD:
    hitCount = booleanQueryShouldMustNot(subs, liveDocsBytes, termScoreCache, termWeights,
                                         maxDoc, topN, numScorers, docBase, numMustNot, filled, docIDs, scores, coords,
                                         topScores, topDocIDs, coordFactors, normTable,
                                         norms, skips, dsSubs, dsCounts, dsMissingDims, dsNumDims, dsTotalHits, dsTermsPerDim, dsHitBits, dsNearMissBits);
  } else if (numMustNot == 0) {
    // At least one MUST and zero or more SHOULD:
    hitCount = booleanQueryShouldMust(subs, liveDocsBytes, termScoreCache, termWeights,
                                      maxDoc, topN, numScorers, docBase, numMust, filled, docIDs, scores, coords,
                                      topScores, topDocIDs, coordFactors, normTable,
                                      norms, dsSubs, dsCounts, dsMissingDims, dsNumDims, dsTotalHits, dsTermsPerDim, dsHitBits, dsNearMissBits);
  } else {
    // At least one MUST_NOT, at least one MUST and zero or more SHOULD:
    hitCount = booleanQueryShouldMustMustNot(subs, liveDocsBytes, termScoreCache, termWeights,
                                             maxDoc, topN, numScorers, docBase, numMust, numMustNot, filled, docIDs, scores, coords,
                                             topScores, topDocIDs, coordFactors, normTable,
                                             norms);
  }

 end:

  if (norms != 0) {
    env->ReleasePrimitiveArrayCritical(jnorms, norms, JNI_ABORT);
  }
  if (liveDocsBytes != 0) {
    env->ReleasePrimitiveArrayCritical(jliveDocsBytes, liveDocsBytes, JNI_ABORT);
  }
  if (normTable != 0) {
    env->ReleasePrimitiveArrayCritical(jnormTable, normTable, JNI_ABORT);
  }
  if (singletonDocIDs != 0) {
    env->ReleaseIntArrayElements(jsingletonDocIDs, singletonDocIDs, JNI_ABORT);
  }
  if (totalTermFreqs != 0) {
    env->ReleaseLongArrayElements(jtotalTermFreqs, totalTermFreqs, JNI_ABORT);
  }
  if (docTermStartFPs != 0) {
    env->ReleaseLongArrayElements(jdocTermStartFPs, docTermStartFPs, JNI_ABORT);
  }
  if (docFreqs != 0) {
    env->ReleaseIntArrayElements(jdocFreqs, docFreqs, JNI_ABORT);
  }
  if (termWeights != 0) {
    env->ReleaseFloatArrayElements(jtermWeights, termWeights, JNI_ABORT);
  }
  if (coordFactors != 0) {
    env->ReleaseFloatArrayElements(jcoordFactors, coordFactors, JNI_ABORT);
  }
  if (topDocIDs != 0) {
    env->ReleaseIntArrayElements(jtopDocIDs, topDocIDs, 0);
  }
  if (topScores != 0) {
    env->ReleaseFloatArrayElements(jtopScores, topScores, 0);
  }
  if (dsCounts != 0) {
    free(dsCounts);
  }
  if (dsMissingDims != 0) {
    free(dsMissingDims);
  }
  if (dsTermsPerDim != 0) {
    env->ReleaseIntArrayElements(jdsTermsPerDim, (int *) dsTermsPerDim, JNI_ABORT);
  }
  if (dsTotalHits != 0) {
    env->ReleaseIntArrayElements(jdsTotalHits, (int *) dsTotalHits, 0);
  }
  if (dsHitBits != 0) {
    env->ReleasePrimitiveArrayCritical(jdsHitBits, dsHitBits, JNI_ABORT);
  }
  if (dsNearMissBits != 0) {
    for(int i=0;i<dsNumDims;i++) {
      if (dsNearMissBits[i] != 0) {
        jlongArray jbits = (jlongArray) env->GetObjectArrayElement(jdsNearMissBits, i);
        env->ReleasePrimitiveArrayCritical(jbits, dsNearMissBits[i], 0);
      }
    }
    free(dsNearMissBits);
  }

  if (termScoreCache != 0) {
    for(int i=0;i<numScorers;i++) {
      if (termScoreCache[i] != 0) {
        free(termScoreCache[i]);
      }
    }
    free(termScoreCache);
  }
  if (filled != 0) {
    free(filled);
  }
  if (docIDs != 0) {
    free(docIDs);
  }
  if (skips != 0) {
    free(skips);
  }
  if (scores != 0) {
    free(scores);
  }
  if (coords != 0) {
    free(coords);
  }
  if (subs != 0) {
    for(int i=0;i<numScorers;i++) {
      PostingsState *sub = subs+i;
      if (sub->docDeltas != 0) {
        free(sub->docDeltas);
      } else if (sub->freqs != 0) {
        free(sub->freqs);
      }
    }

    free(subs);
  }
  if (dsSubs != 0) {
    for(int i=0;i<dsNumDims;i++) {
      PostingsState *sub = dsSubs+i;
      if (sub->docDeltas != 0) {
        free(sub->docDeltas);
      }
    }

    free(dsSubs);
  }
  if (dsDocFreqs != 0) {
    env->ReleaseIntArrayElements(jdsDocFreqs, (int *) dsDocFreqs, JNI_ABORT);
  }
  if (dsDocTermStartFPs != 0) {
    env->ReleaseLongArrayElements(jdsDocTermStartFPs, (long *) dsDocTermStartFPs, JNI_ABORT);
  }
  if (dsSingletonDocIDs != 0) {
    env->ReleaseIntArrayElements(jdsSingletonDocIDs, dsSingletonDocIDs, JNI_ABORT);
  }

  if (failed) {
    jclass c = env->FindClass("java/lang/OutOfMemoryError");
    env->ThrowNew(c, "failed to allocate temporary memory");
    return -1;
  }
  return hitCount;
}


extern "C" JNIEXPORT jint JNICALL
Java_org_apache_lucene_search_NativeSearch_searchSegmentTermQuery
  (JNIEnv *env,
   jclass cl,

   // PQ holding top hits so far, pre-filled with sentinel
   // values: 
   jintArray jtopDocIDs,
   jfloatArray jtopScores,

   // Current segment's maxDoc
   jint maxDoc,

   // Current segment's docBase
   jint docBase,

   // Current segment's liveDocs, or null:
   jbyteArray jliveDocBytes,

   jboolean docsOnly,

   // weightValue from each TermWeight:
   jfloat termWeight,

   // Norms for the field (all TermQuery must be against a single field):
   jbyteArray jnorms,

   // nocommit silly to pass this once for each segment:
   // Cache, mapping byte norm -> float
   jfloatArray jnormTable,

   // If the term has only one docID in this segment (it was
   // "pulsed") then its set here, else -1:
   jint singletonDocID,

   jlong totalTermFreq,

   // docFreq of each term
   jint docFreq,

   // Offset in the .doc file where this term's docs+freqs begin:
   jlong docTermStartFP,

   // Address in memory where .doc file is mapped:
   jlong docFileAddress)
{
  int topN = env->GetArrayLength(jtopDocIDs) - 1;
  //printf("topN=%d\n", topN);

  unsigned char *liveDocBytes = 0;
  unsigned char* norms = 0;
  float *normTable = 0;
  int *topDocIDs = 0;
  float *topScores = 0;
  double *termScoreCache = 0;
  PostingsState *sub = 0;
  int totalHits = 0;
  bool failed = false;

  unsigned char isCopy = 0;
  if (jliveDocBytes == 0) {
    liveDocBytes = 0;
  } else {
    liveDocBytes = (unsigned char *) env->GetPrimitiveArrayCritical(jliveDocBytes, &isCopy);
    if (liveDocBytes == 0) {
      failed = true;
      goto end;
    }
  }

  isCopy = 0;
  norms = (unsigned char *) env->GetPrimitiveArrayCritical(jnorms, &isCopy);
  if (norms == 0) {
    failed = true;
    goto end;
  }

  isCopy = 0;
  normTable = (float *) env->GetPrimitiveArrayCritical(jnormTable, &isCopy);
  if (normTable == 0) {
    failed = true;
    goto end;
  }
  // PQ holding top hits:
  topDocIDs = env->GetIntArrayElements(jtopDocIDs, 0);
  if (topDocIDs == 0) {
    failed = true;
    goto end;
  }

  if (jtopScores == 0) {
    topScores = 0;
  } else {
    topScores = (float *) env->GetFloatArrayElements(jtopScores, 0);
    if (topScores == 0) {
      failed = true;
      goto end;
    }
  }

  termScoreCache = (double *) malloc(TERM_SCORES_CACHE_SIZE*sizeof(double));
  if (termScoreCache == 0) {
    failed = true;
    goto end;
  }
  for(int j=0;j<TERM_SCORES_CACHE_SIZE;j++) {
    termScoreCache[j] = termWeight * sqrt(j);
  }

  if (singletonDocID != -1) {
    if (liveDocBytes == 0 || isSet(liveDocBytes, singletonDocID)) {
      int docID = docBase + singletonDocID;
      if (jtopScores != 0) {
        float score;
        if (totalTermFreq < TERM_SCORES_CACHE_SIZE) {
          score = termScoreCache[totalTermFreq];
        } else {
          score = sqrt(totalTermFreq) * termWeight;
        }
        score *= normTable[norms[singletonDocID]];
        if (score > topScores[1]) {
          topScores[1] = score;
          topDocIDs[1] = docID;
          //printf("singleton docID=%d ttf=%d termWeight=%.5f score=%.7f\n", singletonDocID, totalTermFreq, termWeight, topScores[1]);fflush(stdout);
          downHeap(topN, topDocIDs, topScores);
        } else {
          //printf("skip singleton docID=%d termWeight=%.5f score=%.7f\n", singletonDocID, termWeight, topScores[1]);fflush(stdout);
        }
      } else if (docID < topDocIDs[1]) {
        //printf("singleton docID=%d no score\n", singletonDocID);fflush(stdout);
        topDocIDs[1] = docID;
        downHeapNoScores(topN, topDocIDs);
      } else {
        //printf("skip singleton docID=%d no score\n", singletonDocID);fflush(stdout);
      }
      totalHits = 1;
    }
  } else {
    //printf("TQ: blocks\n");fflush(stdout);

#if 0
    // curiously this more straightforward impl is slower:
    PostingsState *sub = (PostingsState *) malloc(sizeof(PostingsState));
    sub->docsOnly = false;
    //printf("  set docsLeft=%d\n", docFreq);
    sub->docsLeft = docFreq;
    // Locality seemed to help here:
    if (jtopScores != 0) {
      sub->docDeltas = (unsigned int *) malloc(2*BLOCK_SIZE*sizeof(int));
      sub->freqs = sub->docDeltas + BLOCK_SIZE;
    } else {
      sub->docDeltas = (unsigned int *) malloc(BLOCK_SIZE*sizeof(int));
      sub->freqs = 0;
    }
    //printf("docFileAddress=%ld startFP=%ld\n", docFileAddress, docTermStartFPs[i]);fflush(stdout);
    sub->p = ((unsigned char *) docFileAddress) + docTermStartFP;
    int hitCount = 0;

    int nextDocID = 0;
    unsigned int *docDeltas = sub->docDeltas;
    unsigned int *freqs = sub->freqs;

    int blockLastRead = sub->blockLastRead;
    int blockEnd = sub->blockEnd;
  
    if (liveDocBytes != 0) {
      //printf("TQ: has liveDocs\n");fflush(stdout);
      if (topScores == 0) {
        while (sub->docsLeft != 0) {
          nextBlock(sub);
          int limit = sub->blockEnd+1;
          for(int i=0;i<limit;i++) {
            nextDocID += docDeltas[i];
            if (isSet(liveDocBytes, nextDocID)) {
              totalHits++;
              int docID = docBase + nextDocID;
              if (docID < topDocIDs[1]) {
                // Hit is competitive   
                topDocIDs[1] = docID;
                downHeapNoScores(topN, topDocIDs);
              }
            }
          }
        }
      } else {
        while (sub->docsLeft != 0) {
          nextBlock(sub);
          int limit = sub->blockEnd+1;
          //printf("limit=%d\n", limit);
          for(int i=0;i<limit;i++) {
            nextDocID += docDeltas[i];
            if (isSet(liveDocBytes, nextDocID)) {
              totalHits++;

              float score;
              int freq = freqs[i];

              if (freq < TERM_SCORES_CACHE_SIZE) {
                score = termScoreCache[freq];
              } else {
                score = sqrt(freq) * termWeight;
              }

              score *= normTable[norms[nextDocID]];

              int docID = docBase + nextDocID;

              if (score > topScores[1] || (score == topScores[1] && docID < topDocIDs[1])) {
                // Hit is competitive   
                topDocIDs[1] = docID;
                topScores[1] = score;

                downHeap(topN, topDocIDs, topScores);
                //printf("    **\n");fflush(stdout);
              }
            }
          }
        }
      }
    } else {
      //printf("TQ: no liveDocs\n");fflush(stdout);

      if (topScores == 0) {
        //printf("TQ: has topScores\n");fflush(stdout);
        while (sub->docsLeft != 0) {
          nextBlock(sub);
          int limit = sub->blockEnd+1;
          for(int i=0;i<limit;i++) {
            nextDocID += docDeltas[i];

            int docID = docBase + nextDocID;

            // TODO: this is silly: only first topN docs are
            // competitive, after that we should break
            if (docID < topDocIDs[1]) {
              // Hit is competitive   
              topDocIDs[1] = docID;
              downHeapNoScores(topN, topDocIDs);
            }
          }
        }
      } else {
        while (sub->docsLeft != 0) {
          nextBlock(sub);
          int limit = sub->blockEnd+1;
          for(int i=0;i<limit;i++) {
            nextDocID += docDeltas[i];

            float score;
            int freq = freqs[i];

            if (freq < TERM_SCORES_CACHE_SIZE) {
              score = termScoreCache[freq];
            } else {
              score = sqrt(freq) * termWeight;
            }

            score *= normTable[norms[nextDocID]];

            int docID = docBase + nextDocID;

            if (score > topScores[1] || (score == topScores[1] && docID < topDocIDs[1])) {
              // Hit is competitive   
              topDocIDs[1] = docID;
              topScores[1] = score;

              downHeap(topN, topDocIDs, topScores);
              //printf("    **\n");fflush(stdout);
            }
          }
        }
      }

      // No deletions, so totalHits == docFreq
      totalHits = docFreq;
    }

#else

    sub = (PostingsState *) malloc(sizeof(PostingsState));
    if (sub == 0) {
      failed = true;
      goto end;
    }

    sub->docDeltas = 0;
    sub->freqs = 0;
    sub->docsOnly = (bool) docsOnly;
    sub->docsLeft = docFreq;
    // Locality seemed to help here:
    if (jtopScores != 0 && !docsOnly) {
      sub->docDeltas = (unsigned int *) malloc(2*BLOCK_SIZE*sizeof(int));
      if (sub->docDeltas == 0) {
        failed = true;
        goto end;
      }
      sub->freqs = sub->docDeltas + BLOCK_SIZE;
    } else {
      sub->docDeltas = (unsigned int *) malloc(BLOCK_SIZE*sizeof(int));
      if (sub->docDeltas == 0) {
        failed = true;
        goto end;
      }
      sub->freqs = 0;
    }
    //printf("docFileAddress=%ld startFP=%ld\n", docFileAddress, docTermStartFPs[i]);fflush(stdout);
    sub->docFreqs = ((unsigned char *) docFileAddress) + docTermStartFP;
    //printf("  not singleton\n");
    nextDocFreqBlock(sub);
    sub->nextDocID = sub->docDeltas[0];
    //printf("docDeltas[0]=%d\n", sub->docDeltas[0]);
    sub->docFreqBlockLastRead = 0;

    int nextDocID = sub->nextDocID;
    unsigned int *docDeltas = sub->docDeltas;
    unsigned int *freqs = sub->freqs;

    int blockLastRead = sub->docFreqBlockLastRead;
    int blockEnd = sub->docFreqBlockEnd;
  
    if (liveDocBytes != 0) {
      if (topScores == 0) {
        while (true) {

          if (isSet(liveDocBytes, nextDocID)) {
            totalHits++;

            int docID = docBase + nextDocID;

            if (docID < topDocIDs[1]) {
              // Hit is competitive   
              topDocIDs[1] = docID;
              downHeapNoScores(topN, topDocIDs);
            }
          }

          // Inlined nextDoc:
          if (blockLastRead == blockEnd) {
            if (sub->docsLeft == 0) {
              break;
            } else {
              nextDocFreqBlock(sub);
              blockLastRead = -1;
              blockEnd = sub->docFreqBlockEnd;
            }
          }

          nextDocID += docDeltas[++blockLastRead];
        }
      } else if (docsOnly) {
        
        float baseScore = termScoreCache[1];

        while (true) {
          if (isSet(liveDocBytes, nextDocID)) {
            totalHits++;

            float score = baseScore * normTable[norms[nextDocID]];

            int docID = docBase + nextDocID;

            if (score > topScores[1]) {
              // Hit is competitive   
              topDocIDs[1] = docID;
              topScores[1] = score;

              downHeap(topN, topDocIDs, topScores);

              //printf("    **\n");fflush(stdout);
            }
          }

          // Inlined nextDoc:
          if (blockLastRead == blockEnd) {
            if (sub->docsLeft == 0) {
              break;
            } else {
              nextDocFreqBlock(sub);
              blockLastRead = -1;
              blockEnd = sub->docFreqBlockEnd;
            }
          }

          nextDocID += docDeltas[++blockLastRead];
        }
      } else {
        while (true) {
          if (isSet(liveDocBytes, nextDocID)) {
            totalHits++;

            float score;
            int freq = freqs[blockLastRead];

            if (freq < TERM_SCORES_CACHE_SIZE) {
              score = termScoreCache[freq];
            } else {
              score = sqrt(freq) * termWeight;
            }

            score *= normTable[norms[nextDocID]];

            int docID = docBase + nextDocID;

            if (score > topScores[1]) {
              // Hit is competitive   
              topDocIDs[1] = docID;
              topScores[1] = score;

              downHeap(topN, topDocIDs, topScores);

              //printf("    **\n");fflush(stdout);
            }
          }

          // Inlined nextDoc:
          if (blockLastRead == blockEnd) {
            if (sub->docsLeft == 0) {
              break;
            } else {
              nextDocFreqBlock(sub);
              blockLastRead = -1;
              blockEnd = sub->docFreqBlockEnd;
            }
          }

          nextDocID += docDeltas[++blockLastRead];
        }
      }
    } else {

      if (topScores == 0) {
        while (true) {

          int docID = docBase + nextDocID;

          // TODO: this is silly: only first topN docs are
          // competitive, after that we should break
          if (docID < topDocIDs[1]) {
            // Hit is competitive   
            topDocIDs[1] = docID;

            downHeapNoScores(topN, topDocIDs);
          }

          // Inlined nextDoc:
          if (blockLastRead == blockEnd) {
            if (sub->docsLeft == 0) {
              break;
            } else {
              nextDocFreqBlock(sub);
              blockLastRead = -1;
              blockEnd = sub->docFreqBlockEnd;
            }
          }

          nextDocID += docDeltas[++blockLastRead];
        }
      } else if (docsOnly) {

        float baseScore = termScoreCache[1];

        while (true) {

          float score = baseScore * normTable[norms[nextDocID]];
          int docID = docBase + nextDocID;

          if (score > topScores[1]) {
            // Hit is competitive   
            topDocIDs[1] = docID;
            topScores[1] = score;

            downHeap(topN, topDocIDs, topScores);
            //printf("    **\n");fflush(stdout);
          }

          // Inlined nextDoc:
          if (blockLastRead == blockEnd) {
            if (sub->docsLeft == 0) {
              break;
            } else {
              nextDocFreqBlock(sub);
              blockLastRead = -1;
              blockEnd = sub->docFreqBlockEnd;
            }
          }

          nextDocID += docDeltas[++blockLastRead];
        }

      } else {

        while (true) {

          float score;
          int freq = freqs[blockLastRead];

          if (freq < TERM_SCORES_CACHE_SIZE) {
            score = termScoreCache[freq];
          } else {
            score = sqrt(freq) * termWeight;
          }

          score *= normTable[norms[nextDocID]];

          int docID = docBase + nextDocID;

          if (score > topScores[1]) {
            // Hit is competitive   
            topDocIDs[1] = docID;
            topScores[1] = score;

            downHeap(topN, topDocIDs, topScores);
            //printf("    **\n");fflush(stdout);
          }

          // Inlined nextDoc:
          if (blockLastRead == blockEnd) {
            if (sub->docsLeft == 0) {
              break;
            } else {
              nextDocFreqBlock(sub);
              blockLastRead = -1;
              blockEnd = sub->docFreqBlockEnd;
            }
          }

          nextDocID += docDeltas[++blockLastRead];
        }
      }

      totalHits = docFreq;
      //printf("return totalHits=%d\n", docFreq);
    }
#endif
  }

end:
  if (termScoreCache != 0) {
    free(termScoreCache);
  }
  if (sub != 0) {
    if (sub->docDeltas != 0) {
      free(sub->docDeltas);
    }    
    free(sub);
  }
  if (norms != 0) {
    env->ReleasePrimitiveArrayCritical(jnorms, norms, JNI_ABORT);
  }
  if (liveDocBytes != 0) {
    env->ReleasePrimitiveArrayCritical(jliveDocBytes, liveDocBytes, JNI_ABORT);
  }
  if (normTable != 0) {
    env->ReleasePrimitiveArrayCritical(jnormTable, normTable, JNI_ABORT);
  }

  if (topDocIDs != 0) {
    env->ReleaseIntArrayElements(jtopDocIDs, topDocIDs, 0);
  }
  if (topScores != 0) {
    env->ReleaseFloatArrayElements(jtopScores, topScores, 0);
  }

  if (failed) {
    jclass c = env->FindClass("java/lang/OutOfMemoryError");
    env->ThrowNew(c, "failed to allocate temporary memory");
    return -1;
  }

  return totalHits;
}

extern "C" JNIEXPORT void JNICALL
Java_org_apache_lucene_search_NativeSearch_fillMultiTermFilter
  (JNIEnv *env,
   jclass cl,
   jlongArray jbits,
   jbyteArray jliveDocsBytes,
   jlong address,
   jlongArray jtermStats,
   jboolean docsOnly) {

  unsigned char isCopy = 0;

  unsigned char *liveDocsBytes;
  if (jliveDocsBytes == 0) {
    liveDocsBytes = 0;
  } else {
    liveDocsBytes = (unsigned char *) env->GetPrimitiveArrayCritical(jliveDocsBytes, &isCopy);
  }

  isCopy = 0;
  long *bits = (long *) env->GetPrimitiveArrayCritical(jbits, &isCopy);

  long *termStats = (long *) env->GetPrimitiveArrayCritical(jtermStats, 0);

  PostingsState *sub = (PostingsState *) malloc(sizeof(PostingsState));
  sub->docsOnly = (bool) docsOnly;
  sub->docDeltas = (unsigned int *) malloc(BLOCK_SIZE * sizeof(int));
  sub->freqs = 0;

  unsigned int *docDeltas = sub->docDeltas;

  int numTerms = env->GetArrayLength(jtermStats);
  //printf("numTerms=%d\n", numTerms);
  int i = 0;
  while (i < numTerms) {
    sub->docsLeft = (int) termStats[i++];
    sub->docFreqs = (unsigned char *) (address + termStats[i++]);
    nextDocFreqBlock(sub);
    int nextDocID = 0;

    //printf("do term %d docFreq=%d\n", i, sub->docsLeft);fflush(stdout);

    while (true) {
      int limit = sub->docFreqBlockEnd+1;
      //printf("  limit %d\n", limit);fflush(stdout);
      if (liveDocsBytes == 0) {
        for(int j=0;j<limit;j++) {
          nextDocID += docDeltas[j];
          setLongBit(bits, nextDocID);
        }
      } else {
        for(int j=0;j<limit;j++) {
          nextDocID += docDeltas[j];
          if (isSet(liveDocsBytes, nextDocID)) {
            setLongBit(bits, nextDocID);
          }
        }
      }
      if (sub->docsLeft == 0) {
        break;
      } else {
        nextDocFreqBlock(sub);
      }
    }
  }

  free(sub->docDeltas);
  free(sub);
  
  if (jliveDocsBytes != 0) {
    env->ReleasePrimitiveArrayCritical(jliveDocsBytes, liveDocsBytes, JNI_ABORT);
  }
  env->ReleasePrimitiveArrayCritical(jtermStats, termStats, JNI_ABORT);
  env->ReleasePrimitiveArrayCritical(jbits, bits, JNI_ABORT);
}


extern "C" JNIEXPORT jint JNICALL
Java_org_apache_lucene_search_NativeSearch_searchSegmentExactPhraseQuery
  (JNIEnv *env,
   jclass cl,

   // PQ holding top hits so far, pre-filled with sentinel
   // values: 
   jintArray jtopDocIDs,
   jfloatArray jtopScores,

   // Current segment's maxDoc
   jint maxDoc,

   // Current segment's docBase
   jint docBase,

   // Current segment's liveDocs, or null:
   jbyteArray jliveDocsBytes,

   // weightValue from each TermWeight:
   jfloat termWeight,

   // Norms for the field (all TermQuery must be against a single field):
   jbyteArray jnorms,

   // nocommit silly to pass this once for each segment:
   // Cache, mapping byte norm -> float
   jfloatArray jnormTable,

   // If the term has only one docID in this segment (it was
   // "pulsed") then its set here, else -1:
   jintArray jsingletonDocIDs,

   jlongArray jtotalTermFreqs,

   // docFreq of each term
   jintArray jdocFreqs,

   // Offset in the .doc file where this term's docs+freqs begin:
   jlongArray jdocTermStartFPs,

   // Offset in the .doc file where this term's docs+freqs begin:
   jlongArray jposTermStartFPs,

   jintArray jposOffsets,

   // Address in memory where .doc file is mapped:
   jlong docFileAddress,

   // Address in memory where .pos file is mapped:
   jlong posFileAddress,

   jboolean indexHasPayloads,

   jboolean indexHasOffsets)
{
  bool failed = false;
  float *scores = 0;
  int *docIDs = 0;
  unsigned int *coords = 0;
  PostingsState *subs = 0;
  int *singletonDocIDs = 0;
  long *totalTermFreqs = 0;
  long *docTermStartFPs = 0;
  long *posTermStartFPs = 0;
  int *docFreqs = 0;
  unsigned char *liveDocsBytes = 0;
  unsigned char* norms = 0;
  float *normTable = 0;
  int *topDocIDs = 0;
  float *topScores = 0;
  unsigned int *filled = 0;
  double *termScoreCache = 0;
  unsigned char isCopy = 0;
  int *posOffsets = 0;
  int numScorers;
  int topN;
  int hitCount;

  if (jtopScores == 0) {
    scores = 0;
  } else {
    scores = (float *) malloc(CHUNK * sizeof(float));
    if (scores == 0) {
      failed = true;
      goto end;
    }
  }
  coords = (unsigned int *) malloc(CHUNK * sizeof(int));
  if (coords == 0) {
    failed = true;
    goto end;
  }

  docIDs = (int *) malloc(CHUNK * sizeof(int));
  if (docIDs == 0) {
    failed = true;
    goto end;
  }
  for(int i=0;i<CHUNK;i++) {
    docIDs[i] = -1;
  }

  numScorers = env->GetArrayLength(jdocFreqs);

  topN = env->GetArrayLength(jtopDocIDs) - 1;
  //printf("topN=%d\n", topN);

  subs = (PostingsState *) calloc(numScorers, sizeof(PostingsState));
  if (subs == 0) {
    failed = true;
    goto end;
  }
  posOffsets = env->GetIntArrayElements(jposOffsets, 0);
  if (posOffsets == 0) {
    failed = true;
    goto end;
  }
  singletonDocIDs = env->GetIntArrayElements(jsingletonDocIDs, 0);
  if (singletonDocIDs == 0) {
    failed = true;
    goto end;
  }
  totalTermFreqs = env->GetLongArrayElements(jtotalTermFreqs, 0);
  if (totalTermFreqs == 0) {
    failed = true;
    goto end;
  }
  docTermStartFPs = env->GetLongArrayElements(jdocTermStartFPs, 0);
  if (docTermStartFPs == 0) {
    failed = true;
    goto end;
  }
  posTermStartFPs = env->GetLongArrayElements(jposTermStartFPs, 0);
  if (posTermStartFPs == 0) {
    failed = true;
    goto end;
  }
  docFreqs = env->GetIntArrayElements(jdocFreqs, 0);
  if (docFreqs == 0) {
    failed = true;
    goto end;
  }

  liveDocsBytes;
  if (jliveDocsBytes == 0) {
    liveDocsBytes = 0;
  } else {
    liveDocsBytes = (unsigned char *) env->GetPrimitiveArrayCritical(jliveDocsBytes, &isCopy);
    if (liveDocsBytes == 0) {
      failed = true;
      goto end;
    }
    //printf("liveDocs isCopy=%d\n", isCopy);fflush(stdout);
  }

  isCopy = 0;
  norms = (unsigned char *) env->GetPrimitiveArrayCritical(jnorms, &isCopy);
  if (norms == 0) {
    failed = true;
    goto end;
  }

  isCopy = 0;
  normTable = (float *) env->GetPrimitiveArrayCritical(jnormTable, &isCopy);
  if (normTable == 0) {
    failed = true;
    goto end;
  }

  for(int i=0;i<numScorers;i++) {
    PostingsState *sub = &(subs[i]);
    sub->indexHasPayloads = indexHasPayloads;
    sub->indexHasOffsets = indexHasOffsets;
    sub->docsOnly = false;
    sub->id = i;
    //printf("\ninit scorers[%d] of %d\n", i, numScorers);fflush(stdout);

    if (singletonDocIDs[i] != -1) {
      //printf("  singleton: %d\n", singletonDocIDs[i]);
      sub->nextDocID = singletonDocIDs[i];
      sub->docsLeft = 0;
      sub->docFreqBlockLastRead = 0;
      sub->docFreqBlockEnd = 0;
      sub->docDeltas = 0;
      sub->freqs = (unsigned int *) malloc(sizeof(int));
      if (sub->freqs == 0) {
        failed = true;
        goto end;
      }
      sub->freqs[0] = (int) totalTermFreqs[i];
    } else {
      sub->docsLeft = docFreqs[i];
      sub->docDeltas = (unsigned int *) malloc(2*BLOCK_SIZE*sizeof(int));
      if (sub->docDeltas == 0) {
        failed = true;
        goto end;
      }
      // Locality seemed to help here:
      sub->freqs = sub->docDeltas + BLOCK_SIZE;
      //printf("docFileAddress=%ld startFP=%ld\n", docFileAddress, docTermStartFPs[i]);fflush(stdout);
      sub->docFreqs = ((unsigned char *) docFileAddress) + docTermStartFPs[i];
      //printf("  not singleton\n");
      nextDocFreqBlock(sub);
      sub->nextDocID = sub->docDeltas[0];
      //printf("docDeltas[0]=%d\n", sub->docDeltas[0]);
      sub->docFreqBlockLastRead = 0;
    }
    sub->pos = ((unsigned char *) posFileAddress) + posTermStartFPs[i];
    sub->posLeft = totalTermFreqs[i];
    sub->posDeltas = (unsigned int *) malloc(BLOCK_SIZE*sizeof(int));
    if (sub->posDeltas == 0) {
      failed = true;
      goto end;
    }
    nextPosBlock(sub);
    sub->posBlockLastRead = 0;
    //printf("init i=%d nextDocID=%d freq=%d blockEnd=%d singleton=%d\n", i, sub->nextDocID, sub->nextFreq, sub->blockEnd, singletonDocIDs[i]);fflush(stdout);
  }

  // PQ holding top hits:
  topDocIDs = (int *) env->GetIntArrayElements(jtopDocIDs, 0);
  if (topDocIDs == 0) {
    failed = true;
    goto end;
  }

  if (jtopScores != 0) {
    topScores = (float *) env->GetFloatArrayElements(jtopScores, 0);
    if (topScores == 0) {
      failed = true;
      goto end;
    }
  } else {
    topScores = 0;
  }

  filled = (unsigned int *) malloc(CHUNK * sizeof(int));
  if (filled == 0) {
    failed = true;
    goto end;
  }
  termScoreCache = (double *) malloc(TERM_SCORES_CACHE_SIZE*sizeof(double));
  if (termScoreCache == 0) {
    failed = true;
    goto end;
  }
  for(int j=0;j<TERM_SCORES_CACHE_SIZE;j++) {
    termScoreCache[j] = termWeight * sqrt(j);
  }

  hitCount = phraseQuery(subs,
                         liveDocsBytes,
                         termScoreCache,
                         termWeight,
                         maxDoc,
                         topN,
                         numScorers,
                         docBase,
                         filled,
                         docIDs,
                         coords,
                         topScores,
                         topDocIDs,
                         normTable,
                         norms,
                         posOffsets);

  if (hitCount == -1) {
    failed = true;
  }

 end:
  if (norms != 0) {
    env->ReleasePrimitiveArrayCritical(jnorms, norms, JNI_ABORT);
  }
  if (liveDocsBytes != 0) {
    env->ReleasePrimitiveArrayCritical(jliveDocsBytes, liveDocsBytes, JNI_ABORT);
  }
  if (normTable != 0) {
    env->ReleasePrimitiveArrayCritical(jnormTable, normTable, JNI_ABORT);
  }
  if (posOffsets != 0) {
    env->ReleaseIntArrayElements(jposOffsets, posOffsets, JNI_ABORT);
  }
  if (singletonDocIDs != 0) {
    env->ReleaseIntArrayElements(jsingletonDocIDs, singletonDocIDs, JNI_ABORT);
  }
  if (totalTermFreqs != 0) {
    env->ReleaseLongArrayElements(jtotalTermFreqs, totalTermFreqs, JNI_ABORT);
  }
  if (docTermStartFPs != 0) {
    env->ReleaseLongArrayElements(jdocTermStartFPs, docTermStartFPs, JNI_ABORT);
  }
  if (posTermStartFPs != 0) {
    env->ReleaseLongArrayElements(jposTermStartFPs, posTermStartFPs, JNI_ABORT);
  }
  if (docFreqs != 0) {
    env->ReleaseIntArrayElements(jdocFreqs, docFreqs, JNI_ABORT);
  }
  if (topDocIDs != 0) {
    env->ReleaseIntArrayElements(jtopDocIDs, topDocIDs, 0);
  }
  if (topScores != 0) {
    env->ReleaseFloatArrayElements(jtopScores, topScores, 0);
  }
  if (termScoreCache != 0) {
    free(termScoreCache);
  }
  if (filled != 0) {
    free(filled);
  }
  if (docIDs != 0) {
    free(docIDs);
  }
  if (scores != 0) {
    free(scores);
  }
  if (coords != 0) {
    free(coords);
  }
  if (subs != 0) {
    for(int i=0;i<numScorers;i++) {
      PostingsState *sub = &(subs[i]);
      if (sub->docDeltas != 0) {
        free(sub->docDeltas);
      } else if (sub->freqs != 0) {
        free(sub->freqs);
      }
      if (sub->posDeltas != 0) {
        free(sub->posDeltas);
      }
    }

    free(subs);
  }

  if (failed) {
    jclass c = env->FindClass("java/lang/OutOfMemoryError");
    env->ThrowNew(c, "failed to allocate temporary memory");
    return -1;
  }
  return hitCount;
}

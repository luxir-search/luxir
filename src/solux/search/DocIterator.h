#pragma once

// TODO: template based on docid size?

#include <assert.h>
#include <climits>

// TODO: model more as c++ iterator, with end() == INT_MAX?
// That would mean that begin() needs to actually find the first doc
// and that would be a little wasted work if we are going to skip first.
// On the other hand, for single docids, it means less work?

class DocIterator {
public:
  static const int NO_MORE_DOCS = INT_MAX;

   virtual int docID() = 0;
   virtual int nextDoc() = 0;
   virtual int advance(int target) {
     // slow version of advance
     assert(docID() < target);
     int doc;
     do {
       doc = nextDoc();
     } while (doc < target);
     return doc;
   }

   virtual int64_t cost() = 0;
};

// TODO: think about how to efficiently union/intersection iterators
// How to get things to inline?
// Pass *in* a mutable array of integers, and ask for the user to intersect
// Hmmm, or virtualize the array... accept an input iterator and produce an output iterator
// start with the most sparse iterator?  Also consider the ability to efficiently do random access as well though.


#pragma once

#include "solux/search/DocIterator.h"

class PostingsEnum : public DocIterator {



};



// Hmmm, could Fields, Terms, DocsEnum all be templatized
// for (FieldTerms terms : ir.fields()) { ... }
//
// Perhaps have a specific FieldIterator?
// It also seems like FieldInfo (or FieldType) should tie into it
// FieldType - static type info
// FieldInfo - specific to what is actually in the index (could also call it FieldData)

// In Lucene, Fields is only indexed fields?
// IndexReader could get list of all FieldInfo?
// Should that be unified, or maybe the abstraction is good?

// Think about code iterating over all terms!
// How do I avoid a virtual method call for each call to ++
// There must be templates involved somehow?

//
//
// Seems like PointValues, DocValues, IndexedValues should be normalized/combined to some extent...
// like getDocsWithField?
// Hmmm, but a lot of things don't make sense at the top level (i.e. only per-seg)
// What makes sense to be global:
//   FieldType
// Anything else?  Yes! Some stuff only though
//  uniqueTerms: no
//  but min/max values: yes!
//  Should that be a different class though?  TopFieldInfo rather than FieldInfo?
//                                                     should we be able to have a tree of readers like lucene though?  seems like a good idea... which points to a single interface again
//                                            BUT... don't necessarily need base class if accessed via templates!
//


// FieldType - the Schema owns this?
//  what about dynamic fields?  Does a FieldType contain the field name or does each field get it's own type object?
//     - seems like we need own type if we want to be able to specify indexed, stored, etc per field (w/o having to check field and fieldType)
//  - Rather than SchemaField/FieldType in Solr, we *could* just have something like <String+Type> Pair for a SchemaField (all relevant info in the type)
// We may want to avoid instantiating FieldType info per segment?
// FieldInfo is a difference case.
//   could look at FlatBuffers or something to directly represent a FieldInfo?

// What does segment flushing need?  Do that first.  What does merging need?


// IDEA: what about a single "IndexExplorer" class/instance that combines all "Fields" type stuff...
//  - nextField()
//  - nextTerm()
//  - nextDoc()
//  - nextPosition()
//  Maybe not nextField if fields can be encoded differently?
//  If all in one class, does that help inlining???


// PUSH vs PULL???
// INLINING: does push enable better inlining?
//
// passing lambdas to template functions worked really well for inlining!
// what about if the lambda returns control info about when to break (so we could support efficient push, but still break out when skipping?)
// or, the lambda could provide the next docid?


// One idea: don't provide a common base class for termdocs... instead, they must provide a common contract (like c++ iterators, etc, today)
// That would prevent accidental usage of non-virtual methods that won't do the right thing.
//    Actually, just don't have those methods on the true base class, and have virtual methods on the Generic class that inherits from the Base class?
// *But* what about for non-performance sensitive stuff that we want to have some polymorphism?
// Also need a more dynamic/generic for pluggable codecs?

// IDEA: provide upgrades via memory mapped reading of postings?
// (actually use old code to read old index... launch in another process, read the index and provide metadata to this process)
// Support doing this over a socket as well??? (remote termdocs!)  Or grpc / proto3
// What about a simple text format as well?

// For creating a segment... if we did push, then an existing segment and a new TermsHash could have diff implementations that
// But if we did pull, then we could use that same code to search a segment before it's been flushed the first time!

// TODO: apply learnings to indexing chain!!!!
// specifically, for large text fields, we need to not go through any virtual methods (at least avoid it per-token for the most common tokenizers?)
// TODO: try to grok this: http://cpptruths.blogspot.com/2014/08/fun-with-lambdas-c14-style-part-3.html

// For reading anything that will have block encoding of ids/positions, etc, a virtual method call cost can be mitigated
// by bulk decoding into memory
// bulk API: directly exposing deltas would remove the dependency between them (but consuming code would need to add delta.  maybe this is OK?)
// Also try delta-of-delta?
// for merging segments... deltas could be produced and consumed w/o calculating ids?  Maybe just focus on bulk merging at a higher level instead

// IDEA: use a trait somehow in the apply template????

// IDEA: Maybe there should only be a single index reader! (but that would break the idea of using the normal reading mechanism to write the index)


class Fields {



  // TODO: template method that takes a functor and applies it to each term
  // (but need to know underlying implementation and cast it to that to make it efficient and inlined?)
  // could still make it work in the generic case though
};


// What if fields change type across segments?  Support this?

class TermsEnum {

};


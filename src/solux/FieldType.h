#pragma once

#include "index/ByteBlockPool.h"
#include "analysis/Analyzer.h"

#include <memory>


// TODO: where do versions live??? (say we deleted a field... or even changed a field)
// Version based on the table... or based on the DB?

// TODO: what is the default table?  Is there one?  Perhaps we should force a table name / type
// This is the description, or meta-data for a table.
class Table {
public:
  std::string name;

  // a map of fieldName->field
  // pointer to the parent DB?  (how does sharing work? point to all parents?)

};

class DB {
public:
  std::string name;
  // map of tableName->table OR since tables can be shared..
  // map tableName->tableData
  // OR perhaps keep everything at this level "meta-data" and have a parallel data hierarchy
};

// TODO: envision how all this metadata will be serialized and go from there
// Also think about what lookups will need to be done when indexing a doc
// Have a "virtual" database that forwards to partitions???


// TODO: have a FieldInfo object that has a field name, or have a separate FieldType object?
//

class SegFieldIndexed;

// FieldType contains meta-data about a field, including all the information necessary
// to index that field.  It is independent of any given index segment, as it must exist
// before indexing any data.
class FieldType {
public:
  static constexpr int INDEX_DOCS                         = (1<<0);
  static constexpr int INDEX_DOCS_AND_FREQS               = INDEX_DOCS | (1<<1);
  static constexpr int INDEX_DOCS_AND_FREQS_AND_POSITIONS = INDEX_DOCS_AND_FREQS | (1<<2);
  static constexpr int NUM_TOKENS_APPROX                  = (1<<3);
  static constexpr int NUM_TOKENS_EXACT                   = (1<<4);

  // TODO: combine FIELD_LENGTHS with exists... i.e. use "norms" for exists
  // But what about the reverse... the full set of fields that were in a doc?
  // Important for scalability & streaming for a high number of sparse fields?
  // Really only need to store vint of column number in a seg?  Make it optional!
  //
  // Hmmm, what about using convention over configuration for document lengths?
  // For field "foo", we could always store the length in field "foo__len"...
  // Then that could be configured as a numeric column field the way anything else is?
  //   and be defined as implicit so not returned by default???
  // Although at least early on in the indexing chain, we may not want to store that val separately?

  // TODO: add column family?  Here or at a higher level?
  // For that matter, we should prob store a pointer back to the table that contains us?

  // Random idea (along the lines of having a separate field for the length)...
  // have a separate field for everything?
  // IndexedFieldType, LengthFieldType, NumericFieldType, PointFieldType, ...?
  // How else will we avoid storing everything (dimension of points, etc) in this class?
  //
  // What makes sense:
  //  indexed+docvals, indexed+norms+docvals, points+docvals

  std::string name_;
  int flags_;

  virtual ~FieldType() = default;

  const std::string& name() { return name_; }

  // TODO: check standard on cast of int to bool (check generated code too)
  bool indexed() { return (bool) (flags_ & INDEX_DOCS_AND_FREQS_AND_POSITIONS); }
  bool hasFreqs() { return (bool) (flags_ & INDEX_DOCS_AND_FREQS); }
  bool hasPositions() { return (bool) (flags_ & INDEX_DOCS_AND_FREQS_AND_POSITIONS); }
  bool hasNumTokens() { return (bool) (flags_ & (NUM_TOKENS_APPROX | NUM_TOKENS_EXACT)); }

  bool multiValued() { return false; }

  // TODO: how to share analyzers (potentially expensive) among FieldTypes?
  // For example, a dynamic field w/ a big dict for analysis, or
  // even the StopWord list?  dynamic fields will have a template...
  // perhaps that is the answer!
  virtual Analyzer& getAnalyzer() { return *(Analyzer*)0; } // nocommit TODO

  virtual SegFieldIndexed* createSegFieldIndexed(ByteBlockPool& pool);
};


// TODO: who should do the switch based on IndexOptions, and at what point
// i.e. when looping through analysis, and indexing each term, can we avoid a switch on indexing options each time?
enum IndexOptions {
  NONE,  // No inverted index for this field
  DOCS,  // Indexes the documents that match each term
  DOCS_AND_FREQS,  // Indexes the documents as well as how many times each term appeared in the field for the document
  DOCS_AND_FREQS_AND_POSITIONS   // Default for full-text indexing.  Positions allow things like phrase matching
};



class Analyzer;

class FieldInfo {
public:
  std::string name;
  // ? std::string table;

  ~FieldInfo() {}

  IndexOptions indexOptions;

  bool hasNorms() { return true; }
  bool isMultiValued() { return false; }


  Analyzer& getAnalyzer() { return *(Analyzer*)0; } // nocommit


};

class StrFieldInfo : public FieldInfo {
//  std::unique_ptr<FieldValue> valueFromString() {}



};

class TextFieldInfo : public FieldInfo {


};

class IntFieldInfo : public FieldInfo {

};

class DateFieldInfo : public FieldInfo {
  // TODO: any covariant return types?
  //  std::unique_ptr<FieldValue> valueFromString() {}

};

/*

 // How to add a new field type
 FieldType ft = schema.getFieldType("foo")
 FieldType ft = schema.getFieldType("foo", TextFieldType::standard)  // create with this type if not already exist?

 FieldValue fv = makeFieldValue("my field value")
 Doc.add(fv)

 FieldValue sfv = new StringFieldValue(ft, std::string )





 // could make it more opaque?
 Doc.addString(fieldType, string)
 Doc.addInt(fieldType, int)
 Doc.addInts(fieldType, vector<int>)


 // How to index from one thread to many indexers w/o having to allocate a bunch of memory each time?
 // How to prevent same field from being indexed multiple times?


 */

// TODO: getTokenStream() on FieldValue or FieldInfo?
// TODO: perhaps even have FieldInfo make FieldValue instances from generic values?


// TODO: is there a way to do this with types / generic
// add( IntField, 42 )
// templates may be able to work here w/o dynamic memory allocation?
// Of course to pass to another thread you sort of need that....
// index(myIntFieldType, 55)
//


// TODO: Storing a pointer to FieldType/FieldInfo doesn't help much if I have to
// do a lookup to map from FieldType -> SegmentField in the Inverter anyway
//
class FieldValue {
public:
  // what if the type of a field changes?  Maybe the Inverter should be the one to do the lookup, etc?  Make it a weak pointer?  Make it an implementation detail under the covers?
  FieldInfo* fieldInfo;

  virtual ~FieldValue() {}


  // TODO: switch to something more typed... facebook folly dynamic types?  or rapidjson DOM? std::any?
  // TODO: switch to something with multiple values
  std::string value;
  // perhaps a union of values (string, int, int64_t, double, float, etc) based on

  // Have a "fromString"?
  // Have a FieldValue factory on the type????
  // should index() method be on FieldValue( or FieldType?
};

// Do we want inheritance here?
class StrFieldValue : FieldValue {
  std::string val;



};


class Document {
public:
  std::vector< std::unique_ptr<FieldValue> > fields;
};

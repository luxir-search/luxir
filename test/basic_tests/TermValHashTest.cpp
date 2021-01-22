#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/Inverter.h"
#include "solux/index/SegField.h"

using namespace std;
using namespace solux;

TEST(TermValHash, testHash) {

  Inverter inverter;

  FieldType fieldType = FieldType();
  fieldType.name_="text";
  fieldType.flags_ = FieldType::INDEX_DOCS_AND_FREQS_AND_POSITIONS | FieldType::NUM_TOKENS_APPROX;

  Token* token = new Token();
  Tokenizer* tokenizer = new WhitespaceTokenizer(*token);
  auto tokenChain = std::unique_ptr<TokenChain>( new TokenChain(std::unique_ptr<Token>( token ), *tokenizer, std::unique_ptr<TokenStream>(tokenizer)  ) );

  // SegFieldIndexed* segField = new SegFieldDocsFreqPos(*fieldType, inverter.pool_);
  SegFieldIndexed* segField = fieldType.createSegFieldIndexed(inverter.pool_);

  segField->tokenChain = std::move( tokenChain );


  std::string val = "wow";

  // auto pool_size0 = inverter.pool_.size();

  segField->indexTokenStream(0, &val[0], (int)val.size());
  auto pool_size1 = inverter.pool_.size();

  cout << "Inverter size is " << inverter.pool_.size() << endl;
  segField->indexTokenStream(0, &val[0], (int)val.size());
  auto pool_size2 = inverter.pool_.size();

  cout << "Inverter size is " << inverter.pool_.size() << endl;

  // Indexing the same token again should fit within initially allocated streams
  // for the token and hence size should not increase.
  EXPECT_EQ(pool_size1, pool_size2);



  val = "now is the time for all good men to come to the aid of their country ";
  val = "wow";
  val.reserve(10000);
  for (int i=0; i<1000000; i+=1000) {
    val += ' ';
    val += std::to_string(i);
  }

  segField->indexTokenStream(0, &val[0], (int)val.size());

  cout << "Inverter size is " << inverter.pool_.size() << endl;

  for (int i=0; i<1000; i++) {
    segField->indexTokenStream(i+1, &val[0], (int)val.size());
  }

  cout << "Inverter size is " << inverter.pool_.size() << endl;

  // TODO: read back positions


  auto sss = (SegFieldDocsFreqPos*)segField;
  if (sss->termsHash.size() < 100) {
    for (auto p = sss->termsHash.begin(); p != sss->termsHash.end(); ++p) {
      cout << "Term " << *p << endl;
    }

    vector<TermValHash<DocFreqPosStream>::entry_type> terms;
    terms.reserve(sss->termsHash.size());
    std::copy(sss->termsHash.begin(), sss->termsHash.end(), back_inserter(terms));
    cout << "TERMS=" << terms << endl;

    std::sort(terms.begin(), terms.end());
    cout << "SORTED=" << terms << endl;
  }

  delete segField;
}


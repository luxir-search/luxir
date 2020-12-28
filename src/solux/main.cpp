#include <iostream>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <ranges>
#include "index/SegField.h"
#include "solux/util/MemPool.h"
#include "solux/util/TermHash.h"
#include "solux/util/solux_util.h"
#include "index/Inverter.h"
#include "analysis/Analyzer.h"

using namespace std;


// trying to find alternate way to have no virtual calls for each word and enable inline
int main(int argc, char** argv) {
    unused(argc, argv);
    Inverter inverter;
    cout << "Inverter size is " << inverter.pool_.size() << endl;

    FieldType* fieldInfo = new FieldType();
    fieldInfo->name_="text";

    Token* token = new Token();
    Tokenizer* tokenizer = new WhitespaceTokenizer(*token);
    TokenChain* tokenChain = new TokenChain(std::unique_ptr<Token>(token), *tokenizer, std::unique_ptr<TokenStream>(tokenizer) );

    SegFieldIndexed* segField = new SegFieldDocsFreqPos(*fieldInfo, inverter.pool_);
    segField->tokenChain = std::unique_ptr<TokenChain>( tokenChain );

    std::string val = "now is the time for all good men to come to the aid of their country ";
    // val = "wow";
    uint64_t x=0xdeadbeefabadcafe;
    int ntokens=1000;
    for (int i=0; i<ntokens; i++) {
        val += ' ';
        std::string num = std::to_string(x=xorshift(x));
        size_t digits = (uint32_t)(x=xorshift(x)) % 8 + 1;
        num = num.substr(0, std::min(digits, num.size()));
        val += num;
    }

    cout << "String size is " << val.size() << " tokens=" << (ntokens+1) << endl;

    // cout << val << endl;

    segField->indexTokenStream(0, &val[0], (int)val.size());


    cout << "Inverter size is " << inverter.pool_.size() << endl;


    auto start = chrono::steady_clock::now();


    int iter = 100000;
    // int iter = 10;
    for (int i=0; i<iter; i++) {
        segField->indexTokenStream(i+1, &val[0], (int)val.size());  // this is faster
        // ((SegFieldDocsFreqPos*)segField)->indexWhitespace(i+1, &val[0], (int)val.size());  // direct whitespace indexing
    }

    auto end = chrono::steady_clock::now();
    auto sec = chrono::duration<double>(end - start).count();
    auto MB = val.size() * iter / 1000000.0;

    cout << "INDEX TIME: " << chrono::duration<double, milli>(end - start).count() << " ms"  << endl;
    cout << "input MB=" << MB << " MB/sec=" << MB/sec << endl;
    cout << "Inverter size is " << inverter.pool_.size() << endl;

    Document doc;
    cout << doc.fields.size() << endl;
    doc.fields.push_back( make_unique<FieldValue>() );

    unique_ptr<FieldValue> fieldValue = make_unique<FieldValue>();
    doc.fields.push_back( std::move(fieldValue) );

    inverter.index(doc);


    delete fieldInfo;
    delete segField;
    return 0;
}


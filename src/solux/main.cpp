#include <iostream>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <ranges>
#include "index/SegField.h"
#include "solux/util/MemPool.h"
#include "solux/util/TermHash.h"
// #include "rapidjson/document.h"
#include "solux/util/solux_util.h"
#include "index/Inverter.h"
#include "analysis/Analyzer.h"

using namespace std;

std::unordered_set<const void*> allocated;


void test_ranges() {
  auto is_even = [](int const n) {return n % 2 == 0;};

  std::vector<int> lst{55, 4, 66, 3, 77, 2, 88, 1};
  /** seems ranges / views only implemented in g++ for now... (10/2020)
  auto vlst = lst | std::views::filter(is_even) | std::views::reverse ;
  for (auto val : vlst) {
    std::cout << " Got " << val;
  }
  std::cout << std::endl;
  std::ranges::copy(vlst, std::ostream_iterator<int>(std::cout, " "));
  **/

}

// trying to find alternate way to have no virtual calls for each word and enable inline
int main(int argc, char** argv) {
    /***
    std::string s = "  now  is  the  time for all good men to come to the aid of their country";
    WhitespaceTokenizer::process(&s[0],s.size(),
            [=](const char* v, int len){ cout << "GOT TOKEN " << std::string(v,len) << std::endl; }
            );
***/

    /**
   int sz = 1<<18;
   char* prev = nullptr;
   char* first = nullptr;
   for (int i=0; i<100; i++) {
     char* ptr = new char[sz];
     if (prev == nullptr) {
        first = ptr;
     } else {
        cout << "diff=" << ptr-prev << "\txor=" << hex << ((uint64_t)ptr ^ (uint64_t)prev) << dec << endl;
     }
     prev = ptr;
   }
     **/

    test_ranges();

    Inverter inverter;
    cout << "Inverter size is " << inverter.pool_.size() << endl;

    FieldType* fieldInfo = new FieldType();
    fieldInfo->name_="text";

    Token* token = new Token();
    Tokenizer* tokenizer = new WhitespaceTokenizer(*token);
    TokenChain* tokenChain = new TokenChain(std::unique_ptr<Token>(token), *tokenizer, std::unique_ptr<TokenStream>(tokenizer) );

    SegFieldIndexed* segField = new SegFieldDocsFreqPos(*fieldInfo, inverter.pool_);
    segField->tokenChain = std::move( std::unique_ptr<TokenChain>( tokenChain ) );

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

    auto end_size = inverter.pool_.size();


    auto end = chrono::steady_clock::now();
    auto diff1 = (end - start);
    auto diff2 = diff1.count();
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

    /*
    unique_ptr<FieldValue> fieldValue = make_unique<FieldValue>();
    fieldValue->fieldInfo = fieldInfo;
    fieldValue->value="Now is the time for all good men to come to the aid of their country!";


    doc.fields.push_back( std::move(fieldValue) );

    inverter.index(doc);

     ***/

    delete fieldInfo;
    delete segField;
    return 0;
}



struct aaa {
  int64_t x;
  char b;
};

struct bbb {
  aaa one;
  aaa two;
  short sss;
  char three;
};



/**
void testjson() {
  rapidjson::Document document;
  document.Parse("[\"Hello\", 10, {\"a\":\"b\"} ]");
  cout << "TEST: doc[0]==" << document[0].GetString() << endl;



}
**/


void checkzero(void* ptr, size_t sz) {
  char* p = (char*)ptr;
  for (auto i=0; i<sz; i++) {
    if (p[i] != 0) {
      cout << "NOT ZERO: i=" << i << " char=" << (int)p[i] << endl;
      break;
    }
  }

}


// hmmm, it's not my class causing the slowdown... is it pair<>?
// typedef TermHash<char*>::value_type ttt;
// typedef pair<const char*,const char*> ttt;

// this is just as slow as pair<>
class  ttt {
public:
  const char* first;
  const char* second;
};


void checkInit(int sz) {
  MemPool pool;
  TermHash<char*> tbl(pool, 16);

  typedef TermHash<int>::composite_type ttt;
  // ttt* x = new ttt[sz]();
  ttt* x = new ttt[sz];
  auto memsz = sz * sizeof(ttt);
  checkzero(x, memsz);

  memset(&x, 'A', sz * sizeof(ttt));
  delete [] x;

}

int touchRead(ttt* x, int sz) {
  int ret = 0;
  int skipSize = 4096/sizeof(ttt);
  for (int i=0; i<sz; i+=skipSize) {
    ret += x[i].first == nullptr;
  }
  return ret;
}

int touchWrite(ttt* x, int sz) {
  int ret = 0;
  int skipSize = 4096/sizeof(ttt);
  for (int i=0; i<sz; i+=skipSize) {
    ret += x[i].first == nullptr;
    x[i].first = ( const char*)"hello";
  }
  return ret;
}

// This was 2x slower on OS-X w/ clang & g++5 (not on linux with g++4.8 though)
// zeroing memory seems to take the most time... things were 10x as fast when not zeroing.
int timing(int sz) {
   int ret = 0;

    // ttt* x = new ttt[sz]();
    ttt *x = new ttt[sz]();

    ret += touchWrite(x, sz);

    delete[] x;

    return ret;
}

// this is massively faster! is memory not being touched?
// still 2x after touching
int timing2(int sz) {
  int ret = 0;

  ttt* x = (ttt*)calloc(sz, sizeof(ttt));

  ret += touchWrite(x, sz);

  free(x);

  return ret;
}


// test C style allocation w/ new and delete
int timing2a(int sz) {
  int ret = 0;

  // ttt* x = (ttt*)(new char[sz*sizeof(ttt)]);  // this is fastest, but it's not zeroing memory
  ttt* x = (ttt*)(new char[sz*sizeof(ttt)]());

  ret += touchWrite(x, sz);

  delete (char*)x;

  return ret;
}



void testPool() {
  MemPool pool;
  TermHash<int> tbl(pool, 4);

  typedef TermHash<int>::composite_type entry_type;

  entry_type* entry = &tbl.lookupOrAdd("hello",5);
  // assert(entry->first.isNull());
  assert(!entry->first.isNull());
  cout << " isNull==" << entry->first.isNull() << "  " << entry->first << "," << entry->second << endl;
  cout << "entry==" << entry << endl;

  entry->second=1;

  entry_type* entry2 = &tbl.lookupOrAdd("hello",5);
  assert(entry == entry2);
  cout << "entry==" << entry2 << endl;

  tbl.lookupOrAdd("wow",3);
  entry = &tbl.lookupOrAdd("now",3);
  entry->second = 42;
  tbl.lookupOrAdd("brown",4);  // should rehash here...
  tbl.lookupOrAdd("cow",3);

  entry2 = &tbl.lookupOrAdd("now",3);
  assert(entry != entry2);   // diff spot in memory due to the rehash

  assert(entry2->first.equals((const uint8_t*)"now",3));
  cout << "key=" << entry2->first << "val=" << entry2->second << endl;
  assert(entry2->second == 42);
}


int keys=1024*1024;
int64_t minBuild = 1000L*1000*1000*1000*1000;
int64_t minLookup = minBuild;


typedef uint64_t valtype;
inline int64_t val(valtype v) {
  return (int64_t)v;
}
inline valtype newval(int i) {
  return (valtype)i;
}


// TermHash roughly 3 times as fast...
int64_t timePool(int iteration) {
  uint64_t ret = 0;

  MemPool pool;
  TermHash<valtype> tbl(pool, 4);
  // TermHash<uint64_t> tbl(pool, 1<<21);
  typedef TermHash<valtype>::composite_type entry_type;

  unordered_map<std::string, valtype> umap(4);

  std::string key = "12345678901234567890";

  int64_t answer = 0;
  auto start = chrono::steady_clock::now();

  for (unsigned i = 0; i < keys; i++) {
    int len = 4 + (i & 0x0f); // length 4 through 19
    *(int *) (&key[0]) = i;
//    tbl.emplace(&key[0], len, newval(i)); // is emplace better than []?

    //tbl.lookupOrAdd(&key[0], len).second.lastVal = i; // is emplace better than []?

    key.resize(len); umap.emplace(key, newval(i));
    answer += i;
  }

  auto end = chrono::steady_clock::now();
  auto diff1 = (end - start);
  auto diff2 = diff1.count();
  cout << "BUILD TIME: " << chrono::duration<double, milli>(end - start).count() << " ms" << " raw: " << dec << (end - start).count() << endl;
  minBuild = std::min(minBuild, (end-start).count());

  start = chrono::steady_clock::now();

  int64_t answer2 = 0;
    for (unsigned i = 0; i < keys; i++) {
      unsigned idx = (i * 2654435761u) & (keys-1);
      // unsigned idx = i; // sequential access
      int len = 4 + (idx & 0x0f); // length 4 through 19
      *(int *) (&key[0]) = idx;
      answer2 += val( tbl.get(&key[0], len) );
      // key.resize(len); answer2 += val( umap[key] );
    }
    ret += answer2;
    assert(answer = answer2);


  end = chrono::steady_clock::now();
  cout << "LOOKUP TIME: " << chrono::duration<double, milli>(end - start).count() << " ms" << " raw: " << dec << (end - start).count() << endl;
  minLookup = std::min(minLookup, (end-start).count());


  if (iteration == 0) {
    cout << " MAP SIZE == " << umap.size() << endl;
    cout << " TBL SIZE == " << tbl.size() << " pool size=" << pool.size() << endl;
    cout << " answer= " << answer << "  answer2=" << answer2 << endl;
  }

  return ret;
}

// does templating pack things?

int main2() {
  cout << "Hello, World!" << endl;


  MemPool pool;
  cout << pool.BYTE_BLOCK_SIZE << endl;

  cout << sizeof(aaa) << " " << sizeof(bbb) << " sizeof_tuple=" << sizeof( tuple<int64_t,int> ) << endl;

  // teststuff();

  auto start = chrono::steady_clock::now();

  int ret = 0;
  for (int i = 0; i < 10; i++) {
   // ret += timing(1000000);
   ret += timePool(i);
  }
  cout << "build keys/sec=" << (keys*1e9/minBuild) << " lookup keys/sec=" << (keys*1e9/minLookup) << endl;

/**
  std::hash<const char*> hasher;
  cout << "hash=" << hasher("hello") << endl;

  uint64_t myhash = Hash::hash("hello",5);
  // MetroHash64::Hash((uint8_t*)"hello", 5, (uint8_t*)&myhash, 0);
  cout << "hashBytes==" << myhash << endl;

  **/

  auto end = chrono::steady_clock::now();
  auto diff = end - start;
  cout << "TIME: " << chrono::duration <double, milli> (diff).count() << " ms" << " raw: " << dec << diff.count() << endl;
  // cout << "ret=" << ret;



testPool();
  return 0;
}


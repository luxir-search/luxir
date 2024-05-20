#include "PostingsReader.h"

namespace solux {

Postings::DocsCodec Postings::docCodec;
Postings::PositionsCodec Postings::posCodec;
Postings::TFreqCodec& Postings::tfreqCodec = Postings::posCodec;
Postings::NumericCodec Postings::numericCodec;

} // namespace solux
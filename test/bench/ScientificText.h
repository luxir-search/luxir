// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace luxir {

// Deterministic synthetic scientific-article body text (PubMed Central-like):
// mostly ASCII English with mixed case, punctuation, numbers, citations,
// hyphenation, possessives, and a sprinkling of non-ASCII (Greek letters,
// math symbols, accented author names, non-breaking spaces, curly quotes,
// en dashes, an occasional ligature or fullwidth form). Word choice is
// skewed toward the front of the vocabulary, approximating a Zipf
// distribution so a term hash sees realistic repeat rates.
inline std::string scientificText(size_t bytes, uint64_t seed) {
  static constexpr std::string_view words[] = {
    "the", "of", "and", "in", "to", "a", "with", "for", "was", "were", "is", "that", "by", "on",
    "as", "from", "at", "or", "are", "be", "this", "these", "which", "an", "not", "than", "between",
    "patients", "cells", "expression", "study", "protein", "levels", "analysis", "results", "data",
    "treatment", "increased", "significantly", "observed", "showed", "associated", "compared",
    "group", "groups", "control", "response", "using", "after", "during", "both", "however",
    "activity", "effect", "effects", "gene", "genes", "samples", "used", "higher", "lower",
    "function", "mice", "mouse", "human", "clinical", "cancer", "tumor", "disease", "risk",
    "model", "models", "role", "signaling", "pathway", "pathways", "binding", "receptor",
    "receptors", "inhibition", "induced", "regulation", "regulated", "infection", "immune",
    "inflammatory", "cytokine", "cytokines", "previously", "reported", "identified", "including",
    "measured", "performed", "described", "suggesting", "indicating", "demonstrated",
    "mechanisms", "mechanism", "sequencing", "transcription", "factor", "factors", "mutations",
    "mutation", "variants", "phenotype", "genotype", "population", "cohort", "participants",
    "outcomes", "outcome", "mortality", "incidence", "prevalence", "hospital", "therapy",
    "therapeutic", "drug", "drugs", "dose", "doses", "administration", "concentration",
    "concentrations", "temperature", "incubated", "washed", "stained", "antibody", "antibodies",
    "fluorescence", "microscopy", "imaging", "quantified", "normalized", "statistical",
    "differences", "difference", "correlation", "regression", "logistic", "adjusted",
    "confidence", "interval", "intervals", "ratio", "ratios", "hazard", "median", "mean",
    "standard", "deviation", "baseline", "follow-up", "randomized", "trial", "trials",
    "prospective", "retrospective", "cross-sectional", "dose-dependent", "time-dependent",
    "well-known", "high-throughput", "genome-wide", "long-term", "short-term", "in vitro",
    "in vivo", "knockdown", "knockout", "overexpression", "proliferation", "apoptosis",
    "migration", "invasion", "differentiation", "stem", "tissue", "tissues", "blood", "serum",
    "plasma", "liver", "kidney", "brain", "neurons", "neuronal", "mitochondrial", "oxidative",
    "stress", "metabolism", "metabolic", "glucose", "insulin", "lipid", "membrane", "nuclear",
    "cytoplasmic", "localization", "phosphorylation", "ubiquitination", "degradation",
    "stability", "structure", "structural", "domain", "domains", "residues", "complex",
    "complexes", "interaction", "interactions", "network", "networks", "computational",
    "simulations", "predicted", "prediction", "accuracy", "sensitivity", "specificity",
    "classification", "features", "training", "validation", "dataset", "datasets",
    "bacterial", "bacteria", "viral", "virus", "strains", "strain", "resistance", "antibiotic",
    "susceptibility", "isolates", "colonies", "growth", "culture", "medium", "supplemented",
    "children", "adults", "women", "men", "age", "years", "months", "weeks", "days", "hours",
    "respectively", "approximately", "furthermore", "moreover", "although", "whereas",
    "consistent", "inconsistent", "limitations", "future", "studies", "needed", "investigate",
    "underlying", "potential", "novel", "important", "critical", "essential", "specific",
    "patient's", "authors'", "host's", "Parkinson's", "Alzheimer's", "Crohn's",
  };
  static constexpr std::string_view acronyms[] = {
    "DNA", "RNA", "mRNA", "PCR", "qRT-PCR", "IL-6", "IL-1\u03B2", "TNF-\u03B1", "NF-\u03BAB",
    "CD4+", "CD8+", "HIV", "COVID-19", "SARS-CoV-2", "BMI", "HbA1c", "ELISA", "ROC", "AUC",
    "GAPDH", "p53", "EGFR", "TGF-\u03B2", "IFN-\u03B3", "PBS", "DMSO", "SD", "SEM", "CI",
  };
  static constexpr std::string_view names[] = {
    "Smith", "Wang", "Li", "Zhang", "Johnson", "M\u00FCller", "Garc\u00EDa", "S\u00F8rensen",
    "Nakamura", "Kim", "Brown", "Lef\u00E8vre", "\u00C5str\u00F6m", "Dvo\u0159\u00E1k", "Rossi",
  };
  static constexpr std::string_view symbols[] = {
    "\u03B1", "\u03B2", "\u03B3", "\u0394", "\u00B1", "\u2264", "\u2265", "\u00D7", "\u00B5m",
    "\u00B0C", "\u2013", "\u2019", "\uFB01", "\uFF08", "\u2009", "\u00A0",
  };
  constexpr size_t numWords = sizeof(words) / sizeof(words[0]);
  // Long tail: pseudo-words from syllables plus common English suffixes, so a
  // document has a realistic type/token ratio (roughly 0.25-0.3 at 40KB) and
  // the stemmer sees many distinct inflected forms.
  static constexpr std::string_view syllables[] = {
    "ac", "al", "an", "ar", "bi", "ca", "cel", "chro", "co", "cy", "de", "di", "en", "ep", "fi",
    "gen", "glu", "hy", "im", "in", "ka", "lo", "ma", "me", "mi", "mo", "neu", "no", "ox", "pa",
    "pe", "phos", "pro", "ra", "re", "ri", "sa", "se", "sta", "ter", "the", "to", "tra", "ul",
    "va", "ver", "vi", "zo",
  };
  static constexpr std::string_view suffixes[] = {
    "", "", "", "s", "es", "ed", "ing", "ation", "ations", "ity", "ive", "ly", "al", "ic",
    "ment", "ness", "ism", "ize", "ized", "able", "er", "ers", "ence", "ency", "ous", "ase", "in",
  };
  constexpr size_t numSyl = sizeof(syllables) / sizeof(syllables[0]);
  constexpr size_t numSuf = sizeof(suffixes) / sizeof(suffixes[0]);
  auto tailWord = [&](std::string& out, uint64_t id) {
    // a fixed function of id, so the same id is the same word in every doc
    uint64_t h = (id + 1) * 0x9E3779B97F4A7C15ull;
    size_t nsyl = 2 + (size_t) (h % 3);
    for (size_t k = 0; k < nsyl; k++) {
      h = (h ^ (h >> 29)) * 0xBF58476D1CE4E5B9ull;
      out += syllables[(size_t) (h >> 40) % numSyl];
    }
    out += suffixes[(size_t) (h >> 20) % numSuf];
  };

  std::mt19937_64 rng(seed);
  auto pick = [&](size_t n) { return (size_t) (rng() % n); };
  auto zipf = [&]() {
    double u = (double) (rng() >> 11) * (1.0 / 9007199254740992.0);
    return (size_t) ((double) numWords * u * u * u);
  };

  std::string out;
  out.reserve(bytes + 512);
  int sentencesInParagraph = 0;
  while (out.size() < bytes) {
    size_t len = 8 + pick(22);
    for (size_t i = 0; i < len; i++) {
      if (i > 0) out += ' ';
      size_t roll = pick(100);
      if (roll < 5) {
        out += acronyms[pick(sizeof(acronyms) / sizeof(acronyms[0]))];
      } else if (roll < 9) {
        // numbers: integers, decimals, thousands separators, percentages
        switch (pick(5)) {
          case 0: out += std::to_string(pick(1000)); break;
          case 1: out += std::to_string(pick(100)) + "." + std::to_string(pick(100)); break;
          case 2: out += std::to_string(1 + pick(9)) + "," + std::to_string(100 + pick(900)); break;
          case 3: out += std::to_string(pick(100)) + "%"; break;
          default: out += "p < 0.0" + std::to_string(1 + pick(5)); break;
        }
      } else if (roll < 10) {
        out += "(n\u00A0=\u00A0" + std::to_string(10 + pick(990)) + ")";
      } else if (roll < 11) {
        out += "[" + std::to_string(1 + pick(80)) + ", " + std::to_string(1 + pick(80)) + "\u2013" +
               std::to_string(1 + pick(80)) + "]";
      } else if (roll < 12) {
        out += names[pick(sizeof(names) / sizeof(names[0]))];
        out += " et al. (" + std::to_string(1990 + pick(35)) + ")";
      } else if (roll < 13) {
        out += "Fig.\u00A0" + std::to_string(1 + pick(8)) + (char) ('A' + pick(6));
      } else if (roll < 15) {
        std::string_view s = symbols[pick(sizeof(symbols) / sizeof(symbols[0]))];
        out += s;
        if (s.size() == 2 && pick(2)) out += std::to_string(pick(50));
      } else if (roll < 40) {
        // tail ids are Zipf-ish over ~50k pseudo-words
        double u = (double) (rng() >> 11) * (1.0 / 9007199254740992.0);
        tailWord(out, (uint64_t) (50000.0 * u * u * u));
      } else {
        std::string_view w = words[zipf()];
        if (i == 0 || pick(40) == 0) {
          out += (char) (w[0] >= 'a' && w[0] <= 'z' ? w[0] - 32 : w[0]);
          out += w.substr(1);
        } else {
          out += w;
        }
      }
      size_t punct = pick(100);
      if (i + 1 < len) {
        if (punct < 7) out += ',';
        else if (punct < 8) out += ';';
        else if (punct < 9) out += ':';
      }
    }
    out += '.';
    if (++sentencesInParagraph >= 4 + (int) pick(5)) {
      out += "\n\n";
      sentencesInParagraph = 0;
    } else {
      out += ' ';
    }
  }
  return out;
}

}  // namespace luxir

// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/search/FieldComparator.h"
#include "luxir/search/IndexReader.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/log.h"
#include <atomic>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luxir {

class ValueProgram;

enum class StringSortMode : uint8_t {
    DEFAULT,
    GLOBAL,
    SEGMENT
};

class SortField {
public:
    enum SortOrder {
        ASC,
        DESC
    };
    
private:
    std::string fieldName;
    SortOrder order;
    FieldComparator::MissingValue missingValue;
    const FieldType* fieldType;
    StringSortMode stringSortMode;
    // Missing literal (overrides missingValue's edge): the encoded column
    // value for numeric fields, the term for STRING/ID fields.
    std::optional<int64_t> missingNumber;
    std::optional<std::string> missingString;
    FieldComparator::MultiPick pick;
    inline static std::atomic<StringSortMode> stringSortModeOverride =
        StringSortMode::DEFAULT;
    
public:
    // Single constructor that always requires FieldType reference
    SortField(std::string_view field, const FieldType& fieldType, SortOrder order = DESC,
              FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST,
              StringSortMode stringSortMode = configuredStringSortMode())
        : fieldName(std::string(field)),
          order(order),
          missingValue(missing),
          fieldType(&fieldType),
          stringSortMode(stringSortMode),
          pick(FieldComparator::defaultPick(order == DESC)) {
        assert(stringSortMode != StringSortMode::DEFAULT);
        auto& type = const_cast<FieldType&>(fieldType);
        if (type.type() == FieldType::TEXT) {
            throw std::runtime_error("Sort field '" + fieldName +
                "' is TEXT and has no value column; use a string variant");
        }
        if (!type.hasColumn()) {
            throw std::runtime_error("Sort field '" + fieldName + "' has no value column");
        }
    }
    
public:
    
    const std::string& getFieldName() const { return fieldName; }
    const FieldType& getFieldType() const { return *fieldType; }
    SortOrder getOrder() const { return order; }
    bool isReversed() const { 
        // For ascending order, we want smaller values first (not reversed)
        // For descending order, we want larger values first (reversed)
        return order == DESC; 
    }
    FieldComparator::MissingValue getMissingValue() const { return missingValue; }
    const std::optional<int64_t>& getMissingNumber() const { return missingNumber; }
    const std::optional<std::string>& getMissingString() const { return missingString; }
    FieldComparator::MultiPick getPick() const { return pick; }
    void setMissingNumber(int64_t encoded) { missingNumber = encoded; }
    void setMissingString(std::string_view term) { missingString = std::string(term); }
    void setPick(FieldComparator::MultiPick value) { pick = value; }

    // The transformed key (FieldComparator::KeyBatch space: smaller is
    // better) a doc without a value sorts as, for an int-column sort.
    int64_t missingKey() const {
        if (missingNumber.has_value()) {
            return isReversed() ? ~*missingNumber : *missingNumber;
        }
        return missingValue == FieldComparator::MISSING_FIRST
            ? std::numeric_limits<int64_t>::min()
            : std::numeric_limits<int64_t>::max();
    }
    StringSortMode getStringSortMode() const { return stringSortMode; }

    static StringSortMode configuredStringSortMode() {
        StringSortMode override = stringSortModeOverride.load();
        if (override != StringSortMode::DEFAULT) return override;

        // Environment overrides are process settings. Resolve once instead
        // of scanning the environment for every sort specification.
        static const StringSortMode environmentMode = [] {
            const char* configured = std::getenv("LUXIR_STRING_SORT");
            if (configured == nullptr || *configured == '\0' ||
                std::string_view(configured) == "segment") {
                return StringSortMode::SEGMENT;
            }
            if (std::string_view(configured) == "global") {
                return StringSortMode::GLOBAL;
            }
            LOG_WARN("Ignoring invalid LUXIR_STRING_SORT='{}'; expected global or segment",
                     configured);
            return StringSortMode::SEGMENT;
        }();
        return environmentMode;
    }

    static StringSortMode setStringSortModeForTests(StringSortMode value) {
        return stringSortModeOverride.exchange(value);
    }
    
    std::unique_ptr<FieldComparator> createComparator(IndexReader* reader = nullptr) const {
        // Handle regular field types based on FieldType
        FieldType::Type type = const_cast<FieldType*>(fieldType)->type();
        if (FieldType::sortsByIntColumn(type)) {
            // INT and DATE (epoch millis) sort on the raw column values.
            // FLOAT/DOUBLE columns store sortable bits whose int64 order
            // matches the floating point order, so the int comparator
            // works on the encoded values as-is for all four.
            return std::make_unique<SimpleNumericFieldComparator>(
                fieldName, isReversed(), missingValue, missingNumber, pick
            );
        }
        switch (type) {
            case FieldType::Type::ID:
            case FieldType::Type::STRING: {
                // Use FieldType information to determine if this is an indexed string field
                if (const_cast<FieldType*>(fieldType)->isSet(FieldType::INDEX_DOCS)) {
                    // Global ords cannot place a literal that no segment
                    // holds, so a missing literal always compares per segment.
                    if (missingString.has_value() ||
                        (stringSortMode == StringSortMode::SEGMENT &&
                         (!reader || reader->segments().size() > 1))) {
                        return std::make_unique<SegmentOrdComparator>(
                            fieldName, isReversed(), missingValue, missingString, pick
                        );
                    }
                    // Use GlobalOrdComparator for indexed string fields
                    // Build OrdMap only if needed (multi-segment case)
                    std::shared_ptr<OrdMap> ordMap;
                    if (reader && reader->segments().size() > 1) {
                        ordMap = reader->getOrdMap(fieldName);
                    }
                    return std::make_unique<GlobalOrdComparator>(
                        fieldName, ordMap, isReversed(), missingValue, pick
                    );
                } else {
                    // Use StrColComparator for non-indexed string columns
                    return std::make_unique<StrColComparator>(
                        fieldName, isReversed(), missingValue, missingString
                    );
                }
            }

            default:
                throw std::runtime_error("Unknown field type for sorting: " + 
                    std::to_string((int)const_cast<FieldType*>(fieldType)->type()));
        }
    }
};

// Where an EXPR sort clause's missing values sort: an edge, or a literal
// substituted for them (in the expression's int or double type).
struct SortExprMissing {
    FieldComparator::MissingValue edge = FieldComparator::MISSING_LAST;
    bool hasLiteral = false;
    int64_t intValue = 0;
    double doubleValue = 0.0;
};

class SortClause {
public:
    enum Kind {
        COLUMN,
        SCORE,
        DOC,
        EXPR
    };

private:
    Kind kind;
    SortField::SortOrder order;
    std::optional<SortField> sortField;
    const ValueProgram* valueProgram = nullptr;

    SortExprMissing exprMissing;

public:
    using ExprMissing = SortExprMissing;

    explicit SortClause(SortField field)
        : kind(COLUMN), order(field.getOrder()), sortField(std::move(field)) {}

    SortClause(Kind kind, SortField::SortOrder order)
        : kind(kind), order(order) {
        assert(kind == SCORE || kind == DOC);
    }

    SortClause(const ValueProgram& valueProgram, SortField::SortOrder order,
               SortExprMissing missing = {})
        : kind(EXPR), order(order), valueProgram(&valueProgram),
          exprMissing(missing) {}

    const SortExprMissing& getExprMissing() const {
        assert(kind == EXPR);
        return exprMissing;
    }

    Kind getKind() const { return kind; }
    SortField::SortOrder getOrder() const { return order; }
    const SortField& getSortField() const {
        assert(kind == COLUMN);
        return *sortField;
    }
    const ValueProgram& getValueProgram() const {
        assert(kind == EXPR);
        return *valueProgram;
    }
};

struct SortPlan {
    std::vector<SortClause> clauses;
    bool useFieldSort = false;
    bool rankNeedsScores = true;
};

} // namespace luxir

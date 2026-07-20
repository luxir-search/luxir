#pragma once

#include "solux/search/FieldComparator.h"
#include "solux/search/IndexReader.h"
#include "solux/schema/FieldType.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace solux {

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
    
public:
    // Single constructor that always requires FieldType reference
    SortField(std::string_view field, const FieldType& fieldType, SortOrder order = DESC,
              FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST)
        : fieldName(std::string(field)),
          order(order),
          missingValue(missing),
          fieldType(&fieldType) {}
    
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
    
    std::unique_ptr<FieldComparator> createComparator(int numHits, IndexReader* reader = nullptr) const {
        // Handle regular field types based on FieldType
        switch (const_cast<FieldType*>(fieldType)->type()) {
            case FieldType::Type::INT:
            case FieldType::Type::DATE:
            case FieldType::Type::FLOAT:
            case FieldType::Type::DOUBLE:
                // INT and DATE (epoch millis) sort on the raw column values.
                // FLOAT/DOUBLE columns store sortable bits whose int64 order
                // matches the floating point order, so the int comparator
                // works on the encoded values as-is for all four.
                return std::make_unique<SimpleNumericFieldComparator>(
                    fieldName, numHits, isReversed(), missingValue
                );

            case FieldType::Type::ID:
            case FieldType::Type::STRING:
            case FieldType::Type::TEXT: {
                // Use FieldType information to determine if this is an indexed string field
                if (const_cast<FieldType*>(fieldType)->isSet(FieldType::INDEX_DOCS)) {
                    // Use GlobalOrdComparator for indexed string fields
                    // Build OrdMap only if needed (multi-segment case)
                    std::shared_ptr<OrdMap> ordMap;
                    if (reader && reader->segments().size() > 1) {
                        ordMap = reader->getOrdMap(fieldName);
                    }
                    return std::make_unique<GlobalOrdComparator>(
                        fieldName, ordMap, numHits, isReversed(), missingValue
                    );
                } else {
                    // Use StrColComparator for non-indexed string columns
                    return std::make_unique<StrColComparator>(
                        fieldName, numHits, isReversed(), missingValue
                    );
                }
            }

            default:
                throw std::runtime_error("Unknown field type for sorting: " + 
                    std::to_string((int)const_cast<FieldType*>(fieldType)->type()));
        }
    }
};

class SortClause {
public:
    enum Kind {
        COLUMN,
        SCORE,
        DOC
    };

private:
    Kind kind;
    SortField::SortOrder order;
    std::optional<SortField> sortField;

public:
    explicit SortClause(SortField field)
        : kind(COLUMN), order(field.getOrder()), sortField(std::move(field)) {}

    SortClause(Kind kind, SortField::SortOrder order)
        : kind(kind), order(order) {
        assert(kind != COLUMN);
    }

    Kind getKind() const { return kind; }
    SortField::SortOrder getOrder() const { return order; }
    const SortField& getSortField() const {
        assert(kind == COLUMN);
        return *sortField;
    }
};

struct SortPlan {
    std::vector<SortClause> clauses;
    bool useFieldSort = false;
};

} // namespace solux

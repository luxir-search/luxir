#pragma once

#include "solux/search/FieldComparator.h"
#include "solux/search/IndexReader.h"
#include "solux/schema/FieldType.h"
#include <memory>
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
    const FieldType& fieldType_;  // Always required
    
public:
    // Single constructor that always requires FieldType reference
    SortField(std::string_view field, const FieldType& fieldType, SortOrder order = DESC,
              FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST)
        : fieldName(std::string(field)),
          order(order),
          missingValue(missing),
          fieldType_(fieldType) {}
    
public:
    
    const std::string& getFieldName() const { return fieldName; }
    const FieldType& getFieldType() const { return fieldType_; }
    SortOrder getOrder() const { return order; }
    bool isReversed() const { 
        // For ascending order, we want smaller values first (not reversed)
        // For descending order, we want larger values first (reversed)
        return order == DESC; 
    }
    FieldComparator::MissingValue getMissingValue() const { return missingValue; }
    
    std::unique_ptr<FieldComparator> createComparator(int numHits, IndexReader* reader = nullptr) const {
        // Check for special fields
        if (fieldName == "_score_") {
            throw std::runtime_error("Score sorting not yet implemented");
        } else if (fieldName == "_docid_") {
            throw std::runtime_error("Document ID sorting not yet implemented");
        }
        
        // Handle regular field types based on FieldType
        switch (const_cast<FieldType&>(fieldType_).type()) {
            case FieldType::Type::INT:
                return std::make_unique<SimpleNumericFieldComparator>(
                    fieldName, numHits, isReversed(), missingValue
                );
                
            case FieldType::Type::STRING:
            case FieldType::Type::TEXT: {
                // Use FieldType information to determine if this is an indexed string field
                if (const_cast<FieldType&>(fieldType_).isSet(FieldType::INDEX_DOCS)) {
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
            
            case FieldType::Type::FLOAT:
            case FieldType::Type::DOUBLE:
                throw std::runtime_error("Float/Double sorting not yet implemented");
                
            default:
                throw std::runtime_error("Unknown field type for sorting: " + 
                    std::to_string((int)const_cast<FieldType&>(fieldType_).type()));
        }
    }
};

class MultiFieldComparator : public FieldComparator {
    std::vector<std::unique_ptr<FieldComparator>> comparators;
    int numHits;
    
public:
    MultiFieldComparator(const std::vector<SortField>& sortFields, int numHits, IndexReader* reader = nullptr) 
        : numHits(numHits) {
        for (const auto& field : sortFields) {
            auto comp = field.createComparator(numHits, reader);
            if (!comp) {
                throw std::runtime_error("Failed to create comparator for field: " + field.getFieldName());
            }
            comparators.push_back(std::move(comp));
        }
    }
    
    void setSegment(int32_t segment, PostingsReader* reader) override {
        for (auto& comp : comparators) {
            comp->setSegment(segment, reader);
        }
    }
    
    int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
        for (auto& comp : comparators) {
            int cmp = comp->compare(slotA, docA, slotB, docB);
            if (cmp != 0) {
                return cmp;
            }
        }
        // Tiebreaker by segdoc
        return (docA > docB) - (docA < docB);
    }
    
    int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
        for (auto& comp : comparators) {
            int cmp = comp->compareBottom(bottomSlot, bottomDoc, newDoc);
            if (cmp != 0) {
                return cmp;
            }
        }
        return 0;
    }
    
    
    void copy(int32_t slot, segdoc doc) override {
        for (auto& comp : comparators) {
            comp->copy(slot, doc);
        }
    }
    
    
    
    int64_t getComparableValue(int32_t slot) const override {
        // For multi-field sort, we can't represent the comparison as a single value
        // Return the value from the first comparator as an approximation
        if (!comparators.empty()) {
            return comparators[0]->getComparableValue(slot);
        }
        return 0;
    }
    
    
    int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
        assert(dynamic_cast<MultiFieldComparator*>(&other) != nullptr);
        auto* otherMulti = static_cast<MultiFieldComparator*>(&other);
        
        assert(comparators.size() == otherMulti->comparators.size());
        
        for (size_t i = 0; i < comparators.size(); i++) {
            int cmp = comparators[i]->compare(slotA, docA, *otherMulti->comparators[i], slotB, docB);
            if (cmp != 0) {
                return cmp;
            }
        }
        return 0;
    }
    
    void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
        assert(dynamic_cast<MultiFieldComparator*>(&other) != nullptr);
        auto* otherMulti = static_cast<MultiFieldComparator*>(&other);
        
        assert(comparators.size() == otherMulti->comparators.size());
        
        for (size_t i = 0; i < comparators.size(); i++) {
            comparators[i]->copy(slot, *otherMulti->comparators[i], otherSlot, otherDoc);
        }
    }
};

} // namespace solux
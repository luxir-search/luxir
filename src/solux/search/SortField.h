#pragma once

#include "solux/search/FieldComparator.h"
#include "solux/search/IndexReader.h"
#include <memory>
#include <string>
#include <vector>

namespace solux {

class SortField {
public:
    enum Type {
        SCORE,
        DOC,
        INT,
        LONG,
        FLOAT,
        DOUBLE,
        STRING
    };
    
    enum SortOrder {
        ASC,
        DESC
    };
    
private:
    std::string fieldName;
    Type type;
    SortOrder order;
    FieldComparator::MissingValue missingValue;
    
public:
    SortField(std::string_view field, Type type, SortOrder order = DESC,
              FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST)
        : fieldName(std::string(field)), type(type), order(order), missingValue(missing) {}
    
    const std::string& getFieldName() const { return fieldName; }
    Type getType() const { return type; }
    SortOrder getOrder() const { return order; }
    bool isReversed() const { 
        // For ascending order, we want smaller values first (not reversed)
        // For descending order, we want larger values first (reversed)
        return order == DESC; 
    }
    FieldComparator::MissingValue getMissingValue() const { return missingValue; }
    
    std::unique_ptr<FieldComparator> createComparator(int numHits, IndexReader* reader = nullptr) const {
        switch (type) {
            case INT:
            case LONG:
                return std::make_unique<SimpleNumericFieldComparator>(
                    fieldName, numHits, isReversed(), missingValue
                );
            case STRING: {
                // For string sorting, we need OrdMap for multi-segment sorting
                std::shared_ptr<OrdMap> ordMap;
                if (reader && reader->segments().size() > 1) {
                    // Build OrdMap for multi-segment case
                    ordMap = reader->coreIndex().getOrdMap(fieldName);
                }
                return std::make_unique<GlobalOrdComparator>(
                    fieldName, ordMap, numHits, isReversed(), missingValue
                );
            }
            case SCORE:
            case DOC:
            case FLOAT:
            case DOUBLE:
                throw std::runtime_error("Sort type not yet implemented: " + std::to_string(type));
            default:
                throw std::runtime_error("Unknown sort type: " + std::to_string(type));
        }
        return nullptr;
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
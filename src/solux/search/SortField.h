#pragma once

#include "solux/search/FieldComparator.h"
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
    // Constructor taking std::string
    SortField(const std::string& field, Type type, SortOrder order = DESC, 
              FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST)
        : fieldName(field), type(type), order(order), missingValue(missing) {}
        
    // Constructor taking string_view
    SortField(std::string_view field, Type type, SortOrder order = DESC, 
              FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST)
        : fieldName(std::string(field)), type(type), order(order), missingValue(missing) {}
    
    static SortField scoreSort() {
        return SortField(std::string("_score_"), SCORE, DESC);
    }
    
    static SortField docSort() {
        return SortField(std::string("_docid_"), DOC, ASC);
    }
    
    const std::string& getFieldName() const { return fieldName; }
    Type getType() const { return type; }
    SortOrder getOrder() const { return order; }
    bool isReversed() const { 
        // For ascending order, we want smaller values first (not reversed)
        // For descending order, we want larger values first (reversed)
        return order == DESC; 
    }
    FieldComparator::MissingValue getMissingValue() const { return missingValue; }
    
    std::unique_ptr<FieldComparator> createComparator(int numHits) const {
        switch (type) {
            case INT:
            case LONG:
                return std::make_unique<SimpleNumericFieldComparator>(
                    fieldName, numHits, isReversed(), missingValue
                );
            case SCORE:
            case DOC:
            case FLOAT:
            case DOUBLE:
            case STRING:
                throw std::runtime_error("Sort type not yet implemented: " + std::to_string(type));
            default:
                throw std::runtime_error("Unknown sort type: " + std::to_string(type));
        }
        return nullptr;
    }
};

class MultiFieldComparator : public FieldComparator {
private:
    std::vector<std::unique_ptr<FieldComparator>> comparators;
    int numHits;
    
public:
    MultiFieldComparator(const std::vector<SortField>& sortFields, int numHits) 
        : numHits(numHits) {
        for (const auto& field : sortFields) {
            auto comp = field.createComparator(numHits);
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
    
    int compare(int32_t docA, int32_t segA, int32_t docB, int32_t segB) override {
        for (auto& comp : comparators) {
            int cmp = comp->compare(docA, segA, docB, segB);
            if (cmp != 0) {
                return cmp;
            }
        }
        
        segdoc a(segA, docA);
        segdoc b(segB, docB);
        return (a > b) - (a < b);
    }
    
    int compareBottom(int32_t doc, int32_t segment) override {
        for (auto& comp : comparators) {
            int cmp = comp->compareBottom(doc, segment);
            if (cmp != 0) {
                return cmp;
            }
        }
        return 0;
    }
    
    void setBottom(int32_t slot) override {
        for (auto& comp : comparators) {
            comp->setBottom(slot);
        }
    }
    
    void copy(int32_t slot, int32_t doc, int32_t segment) override {
        for (auto& comp : comparators) {
            comp->copy(slot, doc, segment);
        }
    }
    
    bool isReversed() const override {
        // Return the reversed state of the first comparator
        if (!comparators.empty()) {
            return comparators[0]->isReversed();
        }
        return false;
    }
    
    int64_t getValue(int32_t slot) const override {
        // Return the value from the first comparator
        if (!comparators.empty()) {
            return comparators[0]->getValue(slot);
        }
        return 0;
    }
    
    int64_t getDocValue(int32_t docid) override {
        // For multi-field sort, return the value from the first comparator
        if (!comparators.empty()) {
            return comparators[0]->getDocValue(docid);
        }
        return 0;
    }
};

} // namespace solux
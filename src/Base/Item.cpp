/**
   @file
   @author Shin'ichiro Nakaoka
*/

#include "Item.h"
#include "RootItem.h"
#include "ItemPath.h"
#include "ItemManager.h"
#include "ItemClassRegistry.h"
#include "PutPropertyFunction.h"
#include <cnoid/stdx/filesystem>
#include <typeinfo>
#include <bitset>
#include <vector>
#include <map>
#include <unordered_set>
#include <iostream> // for debug
#include "gettext.h"

using namespace std;
using namespace cnoid;
namespace filesystem = cnoid::stdx::filesystem;

namespace {

const bool TRACE_FUNCTIONS = false;

unordered_set<Item*> itemsToEmitSigSubTreeChanged;

int recursiveTreeChangeCounter = 0;
unordered_set<Item*> itemsBeingAddedOrRemoved;
bool isAnyItemInSubTreesBeingAddedOrRemovedSelected = false;

void clearItemsBeingAddedOrRemoved()
{
    itemsBeingAddedOrRemoved.clear();
    isAnyItemInSubTreesBeingAddedOrRemovedSelected = false;
}

bool checkIfAnyItemInSubTreeSelected(Item* item)
{
    if(item->isSelected()){
        return true;
    }
    for(auto child = item->childItem(); child; child = child->nextItem()){
        return checkIfAnyItemInSubTreeSelected(child);
    }
    return false;
}

void memorizeItemBeingAddedOrRemoved(Item* item)
{
    itemsBeingAddedOrRemoved.insert(item);

    if(checkIfAnyItemInSubTreeSelected(item)){
        isAnyItemInSubTreesBeingAddedOrRemovedSelected = true;
    }
}

bool isItemBeingAddedOrRemoved(Item* item)
{
    return itemsBeingAddedOrRemoved.find(item) != itemsBeingAddedOrRemoved.end();
}

}

namespace cnoid {

class Item::Impl
{
public:
    Item* self;
    Item* lastChild;
    bitset<NUM_ATTRIBUTES> attributes;
    vector<bool> checkStates;

    Signal<void(const std::string& oldName)> sigNameChanged;
    Signal<void()> sigDisconnectedFromRoot;
    Signal<void()> sigUpdated;
    Signal<void()> sigPositionChanged;
    Signal<void()> sigSubTreeChanged;

    Signal<void(bool on)> sigSelectionChanged;

    Signal<void(int checkId, bool on)> sigAnyCheckToggled;
    Signal<void(bool on)> sigLogicalSumOfAllChecksToggled;
    map<int, Signal<void(bool on)>> checkIdToSignalMap;

    // for file overwriting management, mainly accessed by ItemManagerImpl
    bool isConsistentWithFile;
    std::string filePath;
    std::string fileFormat;
    std::time_t fileModificationTime;

    Impl(Item* self);
    Impl(Item* self, const Impl& org);
    void initialize();
    ~Impl();
    void setSelected(bool on, bool forceToNotify, bool doEmitSigSelectedItemsChangedLater);
    bool setSubTreeItemsSelectedIter(Item* item, bool on);
    void getDescendantItemsIter(const Item* parentItem, ItemList<>& io_items) const;
    void getSelectedDescendantItemsIter(const Item* parentItem, ItemList<>& io_items) const;
    Item* duplicateSubTreeIter(Item* duplicated) const;
    bool doInsertChildItem(ItemPtr item, Item* newNextItem, bool isManualOperation);
    void doDetachFromParentItem(bool isMoving);
    void callSlotsOnPositionChanged();
    void callFuncOnConnectedToRoot();
    void addToItemsToEmitSigSubTreeChanged();
    static void emitSigSubTreeChanged();
    void emitSigDisconnectedFromRootForSubTree();
    void requestRootItemToEmitSigSelectionChangedForNewlyAddedSelectedItems(Item* item, RootItem* root);
    void requestRootItemToEmitSigCheckToggledForNewlyAddedCheckedItems(Item* item, RootItem* root);
    bool traverse(Item* item, const std::function<bool(Item*)>& function);
};

}


Item::Item()
{
    impl = new Impl(this);
}


Item::Impl::Impl(Item* self)
    : self(self)
{
    initialize();
}


Item::Item(const Item& org)
    : name_(org.name_)
{
    impl = new Impl(this, *org.impl);
}


Item::Impl::Impl(Item* self, const Impl& org)
    : self(self),
      attributes(org.attributes)
{
    initialize();

    if(attributes[LOAD_ONLY]){
        filePath = org.filePath;
        fileFormat = org.fileFormat;
    }
}


void Item::Impl::initialize()
{
    self->classId_ = -1;
    
    self->parent_ = nullptr;
    lastChild = nullptr;
    self->prevItem_ = nullptr;
    self->numChildren_ = 0;
    self->isSelected_ = false;

    attributes.reset(SUB_ITEM);
    attributes.reset(TEMPORAL);

    isConsistentWithFile = false;
    fileModificationTime = 0;
}


Item::~Item()
{
    if(TRACE_FUNCTIONS){
        cout << "Item::~Item() of " << name_ << endl;
    }
    
    Item* child = childItem();
    while(child){
        Item* next = child->nextItem();
        child->detachFromParentItem();
        child = next;
    }

    delete impl;
}


Item::Impl::~Impl()
{
    
}


void Item::validateClassId() const
{
    classId_ = ItemClassRegistry::instance().classId(this);
}


void Item::assign(Item* srcItem)
{
    doAssign(srcItem);
    RootItem* rootItem = findRootItem();
    if(rootItem){
        rootItem->emitSigItemAssinged(this, srcItem);
    }
}


void Item::doAssign(Item* srcItem)
{
    
}


Item* Item::duplicate() const
{
    Item* duplicated = doDuplicate();
    if(duplicated && (typeid(*duplicated) != typeid(*this))){
        delete duplicated;
        duplicated = nullptr;
    }
    return duplicated;
}


Item* Item::doDuplicate() const
{
    return nullptr;
}


Item* Item::duplicateSubTree() const
{
    return impl->duplicateSubTreeIter(nullptr);
}


Item* Item::Impl::duplicateSubTreeIter(Item* duplicated) const
{
    if(!duplicated){
        duplicated = self->duplicate();
    }
    
    if(duplicated){
        for(Item* child = self->childItem(); child; child = child->nextItem()){
            Item* duplicatedChildItem;
            if(child->isSubItem()){
                duplicatedChildItem = duplicated->findChildItem(child->name());
                if(duplicatedChildItem){
                    child->impl->duplicateSubTreeIter(duplicatedChildItem);
                }
            } else {
                duplicatedChildItem = child->impl->duplicateSubTreeIter(nullptr);
                if(duplicatedChildItem){
                    duplicated->addChildItem(duplicatedChildItem);
                }
            }
        }
    }

    return duplicated;
}


void Item::setName(const std::string& name)
{
    if(name != name_){
        string oldName(name_);
        name_ = name;
        impl->sigNameChanged(oldName);
    }
}


SignalProxy<void(const std::string& oldName)> Item::sigNameChanged()
{
    return impl->sigNameChanged;
}


bool Item::hasAttribute(Attribute attribute) const
{
    return impl->attributes[attribute];
}


void Item::setAttribute(Attribute attribute)
{
    impl->attributes.set(attribute);
}


void Item::unsetAttribute(Attribute attribute)
{
    impl->attributes.reset(attribute);
}


bool Item::isSubItem() const
{
    return impl->attributes[SUB_ITEM];
}


bool Item::isTemporal() const
{
    return impl->attributes[TEMPORAL];
}


void Item::setTemporal(bool on)
{
    impl->attributes.set(TEMPORAL, on);
}


void Item::setSelected(bool on, bool isFocused)
{
    impl->setSelected(on, isFocused, true);
}


void Item::Impl::setSelected(bool on, bool isFocused, bool doEmitSigSelectedItemsChangedLater)
{
    if(on != self->isSelected_ || isFocused){
        self->isSelected_ = on;
        sigSelectionChanged(on);
        if(auto rootItem = self->findRootItem()){
            rootItem->emitSigSelectionChanged(self, on, isFocused);
            if(doEmitSigSelectedItemsChangedLater){
                rootItem->emitSigSelectedItemsChangedLater();
            }
        }
    }
}    


void Item::setSubTreeItemsSelected(bool on)
{
    bool changed = impl->setSubTreeItemsSelectedIter(this, on);
    if(changed){
        if(auto rootItem = findRootItem()){
            rootItem->emitSigSelectedItemsChangedLater();
        }
    }
}


bool Item::Impl::setSubTreeItemsSelectedIter(Item* item, bool on)
{
    bool changed = false;
    if(on != item->isSelected()){
        item->impl->setSelected(on, false, false);
        changed = true;
    }
    for(Item* child = item->childItem(); child; child = child->nextItem()){
        if(setSubTreeItemsSelectedIter(child, on)){
            changed = true;
        }
    }
    return changed;
}


bool Item::isChecked(int checkId) const
{
    if(checkId == LogicalSumOfAllChecks){
        for(const auto& checked : impl->checkStates){
            if(checked){
                return true;
            }
        }
        return false;

    } else if(checkId < impl->checkStates.size()){
        return impl->checkStates[checkId];
    }
    
    return false;
}


void Item::setChecked(bool on)
{
    setChecked(PrimaryCheck, on);
}


void Item::setChecked(int checkId, bool on)
{
    if(checkId < 0){
        return;
    }
    
    RootItem* root = nullptr;

    if(checkId >= impl->checkStates.size()){
        root = findRootItem();
        auto tmpRoot = root ? root : RootItem::instance();
        if(checkId >= tmpRoot->numCheckEntries()){
            return;
        }
        impl->checkStates.resize(checkId + 1, false);
    }

    bool current = impl->checkStates[checkId];
    
    if(on != current){

        if(!root){
            root = findRootItem();
        }

        bool doEmitSigLogicalSumOfAllChecksToggled = false;
        bool wasAnyChecked = false;
        for(size_t i=0; i < impl->checkStates.size(); ++i){
            if(impl->checkStates[i]){
                wasAnyChecked = true;
                break;
            }
        }
        if(on != wasAnyChecked){
            doEmitSigLogicalSumOfAllChecksToggled = true;
        }
        
        impl->checkStates[checkId] = on;
        impl->checkIdToSignalMap[checkId](on);
        impl->sigAnyCheckToggled(checkId, on);

        if(doEmitSigLogicalSumOfAllChecksToggled){
            impl->sigLogicalSumOfAllChecksToggled(on);
        }
            
        if(root){
            root->emitSigCheckToggled(this, checkId, on);
            if(doEmitSigLogicalSumOfAllChecksToggled){
                root->emitSigCheckToggled(this, LogicalSumOfAllChecks, on);
            }
        }
    }
}


Item* Item::headItem() const
{
    Item* head = const_cast<Item*>(this);
    while(head->isSubItem()){
        if(head->parent_){
            head = head->parent_;
        } else {
            break;
        }
    }
    return head;
}


/*
Item* Item::rootItem()
{
    return RootItem::instance();
}
*/


RootItem* Item::findRootItem() const
{
    return dynamic_cast<RootItem*>(localRootItem());
}


bool Item::isConnectedToRoot() const
{
    return findRootItem() != nullptr;
}


Item* Item::localRootItem() const
{
    Item* current = const_cast<Item*>(this);
    while(current->parent_){
        current = current->parent_;
    }
    return current;
}


bool Item::addChildItem(Item* item, bool isManualOperation)
{
    return impl->doInsertChildItem(item, nullptr, isManualOperation);
}


bool Item::insertChildItem(Item* item, Item* nextItem, bool isManualOperation)
{
    return impl->doInsertChildItem(item, nextItem, isManualOperation);
}


bool Item::addSubItem(Item* item)
{
    item->impl->attributes.set(SUB_ITEM);
    return addChildItem(item, false);
}


bool Item::insertSubItem(Item* item, Item* nextItem)
{
    item->impl->attributes.set(SUB_ITEM);
    return impl->doInsertChildItem(item, nextItem, false);
}


bool Item::Impl::doInsertChildItem(ItemPtr item, Item* newNextItem, bool isManualOperation)
{
    if(!self->onChildItemAboutToBeAdded(item, isManualOperation)){
        return false; // rejected
    }

    ++recursiveTreeChangeCounter;
    memorizeItemBeingAddedOrRemoved(item);
    
    if(!item->impl->attributes[SUB_ITEM]){
        attributes.reset(TEMPORAL);
    }

    bool isMoving = false;
    RootItem* rootItem = self->findRootItem();

    if(item->parent_){
        RootItem* srcRootItem = item->parent_->findRootItem();
        if(srcRootItem){
            if(srcRootItem == rootItem){
                isMoving = true;
            }
        }
        item->impl->doDetachFromParentItem(isMoving);
    }

    item->parent_ = self;

    if(newNextItem && (newNextItem->parent_ == self)){
        item->nextItem_ = newNextItem;
        Item* prevItem = newNextItem->prevItem_;
        if(prevItem){
            prevItem->nextItem_ = item;
            item->prevItem_ = prevItem;
        } else {
            self->firstChild_ = item;
            item->prevItem_ = nullptr;
        }
        newNextItem->prevItem_ = item;

    } else if(lastChild){
        lastChild->nextItem_ = item;
        item->prevItem_ = lastChild;
        item->nextItem_ = nullptr;
        lastChild = item;
    } else {
        self->firstChild_ = item;
        lastChild = item;
    }

    ++self->numChildren_;

    if(rootItem){
        if(isMoving){
            rootItem->notifyEventOnSubTreeMoved(item);
        } else {
            rootItem->notifyEventOnSubTreeAdded(item);
            item->impl->callFuncOnConnectedToRoot();
            item->impl->requestRootItemToEmitSigSelectionChangedForNewlyAddedSelectedItems(item, rootItem);
            item->impl->requestRootItemToEmitSigCheckToggledForNewlyAddedCheckedItems(item, rootItem);
        }
        if(!isItemBeingAddedOrRemoved(self)){
            item->impl->callSlotsOnPositionChanged();
        }
    }

    addToItemsToEmitSigSubTreeChanged();

    --recursiveTreeChangeCounter;

    if(recursiveTreeChangeCounter == 0){
        emitSigSubTreeChanged();
        if(rootItem && isAnyItemInSubTreesBeingAddedOrRemovedSelected){
            rootItem->emitSigSelectedItemsChangedLater();
        }
        clearItemsBeingAddedOrRemoved();
    }

    return true;
}


bool Item::onChildItemAboutToBeAdded(Item* childItem, bool isManualOperation)
{
    return true;
}


void Item::Impl::callFuncOnConnectedToRoot()
{
    self->onConnectedToRoot();
    for(Item* child = self->childItem(); child; child = child->nextItem()){
        child->impl->callFuncOnConnectedToRoot();
    }
}


void Item::onConnectedToRoot()
{

}


void Item::detachFromParentItem()
{
    ItemPtr self = this;
    impl->doDetachFromParentItem(false);
}


void Item::Impl::doDetachFromParentItem(bool isMoving)
{
    if(!self->parent_){
        return;
    }

    ++recursiveTreeChangeCounter;
    memorizeItemBeingAddedOrRemoved(self);

    // Clear all the selection of the sub tree to remove
    setSubTreeItemsSelectedIter(self, false);

    RootItem* rootItem = self->findRootItem();
  
    if(rootItem){
        rootItem->notifyEventOnSubTreeRemoving(self, isMoving);
    }

    self->parent_->impl->addToItemsToEmitSigSubTreeChanged();
    if(self->prevItem_){
        self->prevItem_->nextItem_ = self->nextItem_;
    } else {
        self->parent_->firstChild_ = self->nextItem_;
    }
    if(self->nextItem_){
        self->nextItem_->prevItem_ = self->prevItem_;
    } else {
        self->parent_->impl->lastChild = self->prevItem_;
    }
    
    --self->parent_->numChildren_;
    self->parent_ = nullptr;
    self->prevItem_ = nullptr;
    self->nextItem_ = nullptr;

    attributes.reset(SUB_ITEM);

    if(rootItem){
        rootItem->notifyEventOnSubTreeRemoved(self, isMoving);
        if(!isMoving){
            if(!isItemBeingAddedOrRemoved(self->parent_)){
                callSlotsOnPositionChanged(); // sigPositionChanged is also emitted
            }
            emitSigDisconnectedFromRootForSubTree();
        }
    }

    --recursiveTreeChangeCounter;

    if(recursiveTreeChangeCounter == 0){
        if(!isMoving){
            emitSigSubTreeChanged();
        }
        if(rootItem && isAnyItemInSubTreesBeingAddedOrRemovedSelected){
            rootItem->emitSigSelectedItemsChangedLater();
        }
        clearItemsBeingAddedOrRemoved();
    }
}


void Item::Impl::callSlotsOnPositionChanged()
{
    self->onPositionChanged();
    sigPositionChanged();

    for(Item* child = self->childItem(); child; child = child->nextItem()){
        child->impl->callSlotsOnPositionChanged();
    }
}


void Item::onPositionChanged()
{

}


void Item::Impl::addToItemsToEmitSigSubTreeChanged()
{
    Item* item = self;
    do {
        itemsToEmitSigSubTreeChanged.insert(item);
        item = item->parentItem();
    } while(item);
}


void Item::Impl::emitSigSubTreeChanged()
{
    vector<Item*> items(itemsToEmitSigSubTreeChanged.size());
    std::copy(itemsToEmitSigSubTreeChanged.begin(), itemsToEmitSigSubTreeChanged.end(), items.begin());
    std::sort(items.begin(), items.end(), [](Item* lhs, Item* rhs){ return !rhs->isOwnedBy(lhs); });
    
    for(auto item : items){
        item->impl->sigSubTreeChanged();
    }
    
    itemsToEmitSigSubTreeChanged.clear();
}


void Item::Impl::emitSigDisconnectedFromRootForSubTree()
{
    for(Item* child = self->childItem(); child; child = child->nextItem()){
        child->impl->emitSigDisconnectedFromRootForSubTree();
    }
    sigDisconnectedFromRoot();

    self->onDisconnectedFromRoot();
}


void Item::onDisconnectedFromRoot()
{
    if(TRACE_FUNCTIONS){
        cout << "Item::onDisconnectedFromRoot() of " << name_ << endl;
    }
}


void Item::Impl::requestRootItemToEmitSigSelectionChangedForNewlyAddedSelectedItems
(Item* item, RootItem* rootItem)
{
    if(item->isSelected()){
        rootItem->emitSigSelectionChanged(item, true, false);
    }
    for(Item* child = item->childItem(); child; child = child->nextItem()){
        requestRootItemToEmitSigSelectionChangedForNewlyAddedSelectedItems(child, rootItem);
    }
}


void Item::Impl::requestRootItemToEmitSigCheckToggledForNewlyAddedCheckedItems
(Item* item, RootItem* root)
{
    auto& checks = item->impl->checkStates;
    int n = checks.size();
    for(int checkId=0; checkId < n; ++checkId){
        if(checks[checkId]){
            root->emitSigCheckToggled(item, checkId, true);
        }
    }
    for(Item* child = item->childItem(); child; child = child->nextItem()){
        requestRootItemToEmitSigCheckToggledForNewlyAddedCheckedItems(child, root);
    }
}


SignalProxy<void()> Item::sigPositionChanged()
{
    return impl->sigPositionChanged;
}


SignalProxy<void()> Item::sigSubTreeChanged()
{
    return impl->sigSubTreeChanged;
}


SignalProxy<void()> Item::sigDisconnectedFromRoot()
{
    return impl->sigDisconnectedFromRoot;
}


SignalProxy<void(bool isSelected)> Item::sigSelectionChanged()
{
    return impl->sigSelectionChanged;
}


SignalProxy<void(int checkId, bool on)> Item::sigAnyCheckToggled()
{
    return impl->sigAnyCheckToggled;
}


SignalProxy<void(bool on)> Item::sigCheckToggled(int checkId)
{
    if(checkId == LogicalSumOfAllChecks){
        return impl->sigLogicalSumOfAllChecksToggled;
    } else {
        return impl->checkIdToSignalMap[checkId];
    }
}
    

static Item* findItemSub(Item* current, ItemPath::iterator it, ItemPath::iterator end)
{
    if(it == end){
        return current;
    }
    Item* item = nullptr;
    for(Item* child = current->childItem(); child; child = child->nextItem()){
        if(child->name() == *it){
            item = findItemSub(child, ++it, end);
            if(item){
                break;
            }
        }
    }
    if(!item){
        for(Item* child = current->childItem(); child; child = child->nextItem()){
            item = findItemSub(child, it, end);
            if(item){
                break;
            }
        }
    }
    return item;
}


Item* Item::find(const std::string& path)
{
    return RootItem::instance()->findItem(path);
}


Item* Item::findItem(const std::string& path) const
{
    ItemPath ipath(path);
    return findItemSub(const_cast<Item*>(this), ipath.begin(), ipath.end());
}


static Item* findChildItemSub(Item* current, ItemPath::iterator it, ItemPath::iterator end)
{
    if(it == end){
        return current;
    }
    Item* item = nullptr;
    for(Item* child = current->childItem(); child; child = child->nextItem()){
        if(child->name() == *it){
            item = findChildItemSub(child, ++it, end);
            if(item){
                break;
            }
        }
    }
    return item;
}


Item* Item::findChildItem(const std::string& path) const
{
    ItemPath ipath(path);
    return findChildItemSub(const_cast<Item*>(this), ipath.begin(), ipath.end());
}


Item* Item::findChildItem(const std::function<bool(Item* item)>& checkType) const
{
    for(auto child = childItem(); child; child = child->nextItem()){
        if(checkType(child)){
            return child;
        }
    }
    return nullptr;
}


static Item* findSubItemSub(Item* current, ItemPath::iterator it, ItemPath::iterator end)
{
    if(it == end){
        return current;
    }
    Item* item = nullptr;
    for(Item* child = current->childItem(); child; child = child->nextItem()){
        if(child->name() == *it && child->isSubItem()){
            item = findSubItemSub(child, ++it, end);
            if(item){
                break;
            }
        }
    }
    return item;
}


Item* Item::findSubItem(const std::string& path) const
{
    ItemPath ipath(path);
    return findSubItemSub(const_cast<Item*>(this), ipath.begin(), ipath.end());
}


bool Item::isOwnedBy(Item* item) const
{
    Item* current = const_cast<Item*>(this);
    while(current->parent_){
        current = current->parent_;
        if(current == item){
            return true;
        }
    }
    return false;
}


ItemList<> Item::childItems() const
{
    ItemList<> items;
    for(auto child = childItem(); child; child = child->nextItem()){
        items.push_back(child);
    }
    return items;
}


ItemList<> Item::descendantItems() const
{
    ItemList<> items;
    impl->getDescendantItemsIter(this, items);
    return items;
}


void Item::Impl::getDescendantItemsIter(const Item* parentItem, ItemList<>& io_items) const
{
    for(auto child = parentItem->childItem(); child; child = child->nextItem()){
        io_items.push_back(child);
        if(child->childItem()){
            getDescendantItemsIter(child, io_items);
        }
    }
}


ItemList<> Item::selectedDescendantItems() const
{
    ItemList<> items;
    impl->getSelectedDescendantItemsIter(this, items);
    return items;
}


void Item::Impl::getSelectedDescendantItemsIter(const Item* parentItem, ItemList<>& io_items) const
{
    for(auto child = parentItem->childItem(); child; child = child->nextItem()){
        if(child->isSelected()){
            io_items.push_back(child);
        }
        if(child->childItem()){
            getSelectedDescendantItemsIter(child, io_items);
        }
    }
}


bool Item::traverse(std::function<bool(Item*)> function)
{
    return impl->traverse(this, function);
}


bool Item::Impl::traverse(Item* item, const std::function<bool(Item*)>& function)
{
    if(function(item)){
        return true;
    }
    for(Item* child = item->childItem(); child; child = child->nextItem()){
        if(traverse(child, function)){
            return true;
        }
    }
    return false;
}


void Item::notifyUpdate()
{
    impl->sigUpdated();
}


SignalProxy<void()> Item::sigUpdated()
{
    return impl->sigUpdated;
}


bool Item::load(const std::string& filename, const std::string& format)
{
    return ItemManager::load(this, filename, parentItem(), format);
}


bool Item::load(const std::string& filename, Item* parent, const std::string& format)
{
    return ItemManager::load(this, filename, parent, format);
}


bool Item::save(const std::string& filename, const std::string& format)
{
    return ItemManager::save(this, filename, format);
}


bool Item::overwrite(bool forceOverwrite, const std::string& format)
{
    return ItemManager::overwrite(this, forceOverwrite, format);
}


const std::string& Item::filePath() const
{
    return impl->filePath;
}


const std::string& Item::fileFormat() const
{
    return impl->fileFormat;
}


#ifdef CNOID_BACKWARD_COMPATIBILITY
const std::string& Item::lastAccessedFilePath() const
{
    return impl->filePath;
}


const std::string& Item::lastAccessedFileFormatId() const
{
    return impl->fileFormat;
}
#endif


std::time_t Item::fileModificationTime() const
{
    return impl->fileModificationTime;
}


bool Item::isConsistentWithFile() const
{
    return impl->isConsistentWithFile;
}


void Item::setConsistentWithFile(bool isConsistent)
{
    impl->isConsistentWithFile = isConsistent;
}


void Item::suggestFileUpdate()
{
    impl->isConsistentWithFile = false;
}


void Item::updateFileInformation(const std::string& filename, const std::string& format)
{
    filesystem::path fpath(filename);
    if(filesystem::exists(fpath)){
        impl->fileModificationTime = filesystem::last_write_time_to_time_t(fpath);
        impl->isConsistentWithFile = true;
    } else {
        impl->fileModificationTime = 0;
        impl->isConsistentWithFile = false;
    }        
    impl->filePath = filename;
    impl->fileFormat = format;
}


void Item::clearFileInformation()
{
    impl->filePath.clear();
    impl->fileFormat.clear();
    impl->isConsistentWithFile = true;
}


void Item::putProperties(PutPropertyFunction& putProperty)
{
    putProperty(_("Name"), name_,
                [&](const string& name){
                    if(!name.empty()){
                        setName(name);
                        return true;
                    }
                    return false;
                });

    std::string moduleName, className;
    ItemManager::getClassIdentifier(this, moduleName, className);
    putProperty(_("Class"), className);
    
    doPutProperties(putProperty);

    if(!impl->filePath.empty()){
        putProperty(_("File"), FilePathProperty(impl->filePath));
    }

    putProperty(_("Num children"), numChildren_);
    putProperty(_("Sub item?"), isSubItem());
    putProperty(_("Temporal"), isTemporal());
    putProperty(_("Refs"), refCount());
}


void Item::doPutProperties(PutPropertyFunction& putProperty)
{

}


bool Item::store(Archive& archive)
{
    return true;
}


bool Item::restore(const Archive& archive)
{
    return true;
}

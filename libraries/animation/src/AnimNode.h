//
//  AnimInterface.h
//
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AnimNode_h
#define hifi_AnimNode_h

#include <string>
#include <vector>
#include <memory>
#include <cassert>

typedef float AnimPose;
class QJsonObject;

class AnimNode {
public:
    enum Type {
        ClipType = 0,
        NumTypes
    };

    AnimNode(Type type, const std::string& id) : _type(type), _id(id) {}

    const std::string& getID() const { return _id; }
    Type getType() const { return _type; }

    void addChild(std::shared_ptr<AnimNode> child) { _children.push_back(child); }
    void removeChild(std::shared_ptr<AnimNode> child) {
        auto iter = std::find(_children.begin(), _children.end(), child);
        if (iter != _children.end()) {
            _children.erase(iter);
        }
    }
    const std::shared_ptr<AnimNode>& getChild(int i) const {
        assert(i >= 0 && i < (int)_children.size());
        return _children[i];
    }
    int getChildCount() const { return (int)_children.size(); }

    virtual ~AnimNode() {}

    virtual const AnimPose& evaluate(float dt) = 0;

protected:
    std::string _id;
    Type _type;
    std::vector<std::shared_ptr<AnimNode>> _children;

    // no copies
    AnimNode(const AnimNode&) = delete;
    AnimNode& operator=(const AnimNode&) = delete;
};

#endif // hifi_AnimNode_h

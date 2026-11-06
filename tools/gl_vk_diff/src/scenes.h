#pragma once
#include <string>

#include "igl.h"

namespace gvd {

struct Scene {
    const char* name;       // 01_clear 等，用于目录与过滤
    const char* title;      // 人话标题
    const char* exercises;  // 覆盖的 GL 语义
    const char* hint;       // 失败时最可能的原因提示
    float clear[4];
    // 返回 false 表示该后端跑不了这个场景，note 里写原因。
    bool (*run)(IGl& gl, int w, int h, std::string& note);
};

int SceneCount();
const Scene& SceneAt(int index);
const Scene* FindScene(const std::string& name);

const char* StatusName(Status s);

}  // namespace gvd

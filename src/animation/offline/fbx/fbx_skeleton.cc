//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2019 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/offline/fbx/fbx_skeleton.h"

#include "ozz/animation/offline/raw_skeleton.h"

#include "ozz/base/log.h"

namespace ozz {
namespace animation {
namespace offline {
namespace fbx {

namespace {

enum RecurseReturn { kError, kSkeletonFound, kNoSkeleton };

bool IsTypeSelected(const OzzImporter::NodeType& _types,
                    FbxNodeAttribute::EType _node_type) {
  // Early out to accept any node type
  if (_types.any) {
    return true;
  }

  switch (_node_type) {
    // Skeleton
    case FbxNodeAttribute::eSkeleton:
      return _types.skeleton;

    // Marker
    case FbxNodeAttribute::eMarker:
      return _types.marker;

    // Geometry
    case FbxNodeAttribute::eMesh:
    case FbxNodeAttribute::eNurbs:
    case FbxNodeAttribute::ePatch:
    case FbxNodeAttribute::eNurbsCurve:
    case FbxNodeAttribute::eTrimNurbsSurface:
    case FbxNodeAttribute::eBoundary:
    case FbxNodeAttribute::eNurbsSurface:
    case FbxNodeAttribute::eShape:
    case FbxNodeAttribute::eSubDiv:
    case FbxNodeAttribute::eLine:
      return _types.geometry;

    // Camera
    case FbxNodeAttribute::eCameraStereo:
    case FbxNodeAttribute::eCamera:
      return _types.camera;

    // Light
    case FbxNodeAttribute::eLight:
      return _types.light;

    // Others
    case FbxNodeAttribute::eUnknown:
    case FbxNodeAttribute::eNull:
    case FbxNodeAttribute::eCameraSwitcher:
    case FbxNodeAttribute::eOpticalReference:
    case FbxNodeAttribute::eOpticalMarker:
    case FbxNodeAttribute::eCachedEffect:
    case FbxNodeAttribute::eLODGroup:
    default:
      return false;
  }
}

RecurseReturn RecurseNode(FbxNode* _node,
                          const ozz::Vector<FbxCluster*>::Std& _clusters,
                          FbxSceneLoader& _loader,
                          const OzzImporter::NodeType& _types,
                          RawSkeleton* _skeleton, RawSkeleton::Joint* _parent,
                          FbxAMatrix _parent_global_inv) {
  bool skeleton_found = false;
  RawSkeleton::Joint* this_joint = NULL;

  // Process this node as a new joint if it has a joint compatible attribute.
  FbxNodeAttribute* node_attribute = _node->GetNodeAttribute();
  if (node_attribute &&
      IsTypeSelected(_types, node_attribute->GetAttributeType())) {
    skeleton_found = true;

    RawSkeleton::Joint::Children* sibling = NULL;
    if (_parent) {
      sibling = &_parent->children;
    } else {
      sibling = &_skeleton->roots;
    }

    // Adds a new child.
    sibling->resize(sibling->size() + 1);
    this_joint = &sibling->back();  // Will not be resized inside recursion.
    this_joint->name = _node->GetName();

    // Extract bind pose.
    FbxAMatrix node_global = _node->EvaluateGlobalTransform();

    bool node_global_found = false;
    for (int cluster_index = 0; cluster_index < _clusters.size(); ++cluster_index) {
      FbxCluster* cluster = _clusters[cluster_index];
      if (cluster->GetLink() == _node) {
        cluster->GetTransformLinkMatrix(node_global);
        node_global_found = true;
        break;
      }
    }

    if (!node_global_found) {
      const int pose_count = _loader.scene()->GetPoseCount();
      for (int i = 0; i < pose_count; ++i) {
        auto pose = _loader.scene()->GetPose(i);
        if (pose && pose->IsBindPose()) {
          int found_node = pose->Find(_node);
          if (found_node >= 0) {
            FbxMatrix none_affine_matrix = pose->GetMatrix(found_node);
            node_global = *(FbxAMatrix*)(double*)&none_affine_matrix;
            break;
          }
        }
      }
    }

    const FbxAMatrix node_local = _parent_global_inv * node_global;

    if (!_loader.converter()->ConvertTransform(node_local, &this_joint->transform)) {
      ozz::log::Err() << "Failed to extract skeleton transform for joint \""
                      << this_joint->name << "\"." << std::endl;
      return kError;
    }

    // This node is the new parent for further recursions.
    _parent_global_inv = node_global.Inverse();
    _parent = this_joint;
  }

  // Iterate node's children, even if this one wasn't processed.
  for (int i = 0; i < _node->GetChildCount(); i++) {
    FbxNode* child = _node->GetChild(i);
    const RecurseReturn ret = RecurseNode(child, _clusters, _loader, _types,
                                          _skeleton, _parent, _parent_global_inv);
    if (ret == kError) {
      return ret;
    }
    skeleton_found |= (ret == kSkeletonFound);
  }

  return skeleton_found ? kSkeletonFound : kNoSkeleton;
}

void CollectClusters(FbxNode* _node, ozz::Vector<FbxCluster*>::Std& _clusters) {
  FbxMesh* fbx_mesh = _node->GetMesh();
  if (fbx_mesh) {
    const int skin_deformer_count = fbx_mesh->GetDeformerCount(FbxDeformer::eSkin);
    for (int deformer_index = 0; deformer_index < skin_deformer_count; ++deformer_index) {
      FbxSkin* skin = (FbxSkin*)fbx_mesh->GetDeformer(deformer_index, FbxDeformer::eSkin);
      for (int cluster_index = 0; cluster_index < skin->GetClusterCount(); ++cluster_index) {
        FbxCluster* cluster = skin->GetCluster(cluster_index);
        if (cluster) {
          _clusters.push_back(cluster);
        }
      }
    }
  }

  // Iterate node's children, even if this one wasn't processed.
  for (int i = 0; i < _node->GetChildCount(); i++) {
    FbxNode* child = _node->GetChild(i);
    CollectClusters(child, _clusters);
  }
}
}  // namespace

bool ExtractSkeleton(FbxSceneLoader& _loader,
                     const OzzImporter::NodeType& _types,
                     RawSkeleton* _skeleton) {
  ozz::Vector<FbxCluster*>::Std clusters;
  CollectClusters(_loader.scene()->GetRootNode(), clusters);
  RecurseReturn ret =
      RecurseNode(_loader.scene()->GetRootNode(), clusters, _loader,
                  _types, _skeleton, NULL, FbxAMatrix());
  if (ret == kNoSkeleton) {
    ozz::log::Err() << "No skeleton found in Fbx scene." << std::endl;
    return false;
  } else if (ret == kError) {
    ozz::log::Err() << "Failed to extract skeleton." << std::endl;
    return false;
  }
  return true;
}
}  // namespace fbx
}  // namespace offline
}  // namespace animation
}  // namespace ozz

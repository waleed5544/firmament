// The Firmament project
// Copyright (c) 2013 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
// Copyright (c) 2015 Ionel Gog <ionel.gog@cl.cam.ac.uk>
//

#include "scheduling/flow/flow_graph_manager.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <boost/bind.hpp>

#include "base/common.h"
#include "base/types.h"
#include "misc/map-util.h"
#include "misc/pb_utils.h"
#include "misc/string_utils.h"
#include "misc/utils.h"
#include "scheduling/flow/cost_model_interface.h"
#include "scheduling/flow/dimacs_add_node.h"
#include "scheduling/flow/dimacs_change_arc.h"
#include "scheduling/flow/dimacs_new_arc.h"
#include "scheduling/flow/dimacs_remove_node.h"
#include "scheduling/flow/flow_graph.h"

DEFINE_bool(preemption, false, "Enable preemption and migration of tasks");

namespace firmament {

FlowGraphManager::FlowGraphManager(
    CostModelInterface *cost_model,
    unordered_set<ResourceID_t,
    boost::hash<boost::uuids::uuid>>* leaf_res_ids,
    TimeInterface* time_manager,
    TraceGenerator* trace_generator,
    DIMACSChangeStats* dimacs_stats)
    : cost_model_(cost_model),
      flow_graph_(new FlowGraph),
      leaf_res_ids_(leaf_res_ids),
      trace_generator_(trace_generator),
      dimacs_stats_(dimacs_stats),
      cur_traversal_counter_(0) {
  // Add sink and cluster aggregator node
  AddSpecialNodes();
}

FlowGraphManager::~FlowGraphManager() {
  // We don't delete cost_model_, trace_generator_ and dimacs_stats_ because
  // they are owned by the FlowScheduler.
  delete flow_graph_;
  ResetChanges();
  // XXX(malte): N.B. this leaks memory as we haven't destroyed all of the
  // nodes and arcs in the flow graph (which are allocated on the heap)
}

void FlowGraphManager::AddArcsForTask(FlowGraphNode* task_node,
                                      FlowGraphNode* unsched_agg_node,
                                      vector<FlowGraphArc*>* task_arcs) {
  // NOTE: We do not create DIMACS changes while adding the arcs because
  // we can add all the arcs in one go when we create a new node. Otherwise,
  // if we just update an existing node then the code calling this method
  // must generate DIMACS changes.

  // Cost model may need to do some setup for newly added tasks
  cost_model_->AddTask(task_node->task_id_);
  // We always have an edge to our job's unscheduled node
  FlowGraphArc* unsched_arc = flow_graph_->AddArc(task_node, unsched_agg_node);
  // Add this task's potential flow to the per-job unscheduled
  // aggregator's outgoing edge
  UpdateUnscheduledAggToSinkCapacity(task_node->job_id_, 1);
  // Assign cost to the (task -> unscheduled agg) edge from cost model
  unsched_arc->cost_ =
      cost_model_->TaskToUnscheduledAggCost(task_node->task_id_);
  // Set up arc to unscheduled aggregator
  unsched_arc->cap_upper_bound_ = 1;
  dimacs_stats_->UpdateStats(ADD_ARC_TO_UNSCHED);
  task_arcs->push_back(unsched_arc);
  vector<ResourceID_t>* task_pref_arcs =
    cost_model_->GetTaskPreferenceArcs(task_node->task_id_);
  // Nothing to do if there are no task preference arcs for this task
  if (!task_pref_arcs)
    return;
  // Otherwise add the arcs
  for (auto& pref_res_id : *task_pref_arcs) {
    FlowGraphArc* arc_to_res =
      flow_graph_->AddArc(task_node, NodeForResourceID(pref_res_id));
    arc_to_res->cost_ =
      cost_model_->TaskToResourceNodeCost(task_node->task_id_, pref_res_id);
    arc_to_res->cap_upper_bound_ = 1;
    dimacs_stats_->UpdateStats(ARC_ARC_TASK_TO_RES);
    task_arcs->push_back(arc_to_res);
  }
  delete task_pref_arcs;
}

void FlowGraphManager::AddArcsFromToOtherEquivNodes(EquivClass_t equiv_class,
                                                    FlowGraphNode* ec_node) {
  pair<vector<EquivClass_t>*,
       vector<EquivClass_t>*> equiv_class_to_connect =
    cost_model_->GetEquivClassToEquivClassesArcs(equiv_class);
  // Add incoming arcs.
  if (equiv_class_to_connect.first) {
    for (auto& src_equiv_class : *equiv_class_to_connect.first) {
      FlowGraphNode* ec_src_ptr = FindPtrOrNull(tec_to_node_, src_equiv_class);
      CHECK_NOTNULL(ec_src_ptr);
      FlowGraphArc* arc = flow_graph_->AddArc(ec_src_ptr->id_, ec_node->id_);
      arc->cost_ =
        cost_model_->EquivClassToEquivClass(src_equiv_class, equiv_class);
      // We set the capacity to the max of the source EC's incoming capacity and
      // the destination EC's outgoing capacity. This works, although it's
      // not optimal: we could use the min, to give tighter bounds to the
      // solver, but doing so would require us to dynamically update the
      // capacities at runtime, which we currently don't.
      // Such dynamic updates may, however, still be required even with the
      // current model when more than two layers of ECs are connected.
      // ---
      // The capacity on the arc is max(sum(src_in_caps), sum(dst_out_caps))
      arc->cap_upper_bound_ = CapacityBetweenECNodes(*ec_src_ptr, *ec_node);
      DIMACSChange *chg = new DIMACSNewArc(*arc);
      chg->set_comment("AddArcsFromToOtherEquivNodes: incoming");
      dimacs_stats_->UpdateStats(ADD_ARC_BETWEEN_EQUIV_CLASS);
      AddGraphChange(chg);
    }
    delete equiv_class_to_connect.first;
  }
  // Add outgoing arcs.
  if (equiv_class_to_connect.second) {
    for (auto& dst_equiv_class : *equiv_class_to_connect.second) {
      FlowGraphNode* ec_dst_ptr = FindPtrOrNull(tec_to_node_, dst_equiv_class);
      CHECK_NOTNULL(ec_dst_ptr);
      FlowGraphArc* arc = flow_graph_->AddArc(ec_node->id_, ec_dst_ptr->id_);
      arc->cost_ =
        cost_model_->EquivClassToEquivClass(equiv_class, dst_equiv_class);
      // We set the capacity to the max of the source EC's incoming capacity and
      // the destination EC's outgoing capacity. This works, although it's
      // not optimal: we could use the min, to give tighter bounds to the
      // solver, but doing so would require us to dynamically update the
      // capacities at runtime, which we currently don't.
      // Such dynamic updates may, however, still be required even with the
      // current model when more than two layers of ECs are connected.
      // ---
      // The capacity on the arc is max(sum(src_in_caps), sum(dst_out_caps))
      arc->cap_upper_bound_ = CapacityBetweenECNodes(*ec_node, *ec_dst_ptr);
      DIMACSChange *chg = new DIMACSNewArc(*arc);
      chg->set_comment("AddArcsFromToOtherEquivNodes: outgoing");
      dimacs_stats_->UpdateStats(ADD_ARC_BETWEEN_EQUIV_CLASS);
      AddGraphChange(chg);
    }
    delete equiv_class_to_connect.second;
  }
}

FlowGraphNode* FlowGraphManager::AddEquivClassNode(EquivClass_t ec) {
  VLOG(2) << "Add equiv class " << ec;
  vector<FlowGraphArc*> ec_arcs;
  // Add the equivalence class flow graph node.
  FlowGraphNode* ec_node = flow_graph_->AddNode();
  ec_node->type_ = FlowNodeType::EQUIVALENCE_CLASS;
  ec_node->ec_id_ = ec;
  CHECK(InsertIfNotPresent(&tec_to_node_, ec, ec_node));
  string comment;
  spf(&comment, "EC_AGG_%ju", ec);
  ec_node->comment_ = comment;
  // Add arcs for the new EC
  AddOrUpdateEquivClassArcs(ec, &ec_arcs);
  // Add the new equivalence node to the graph changes
  DIMACSChange *chg = new DIMACSAddNode(*ec_node, ec_arcs);
  chg->set_comment("AddEquivClassNode");
  dimacs_stats_->UpdateStats(ADD_EQUIV_CLASS_NODE);
  AddGraphChange(chg);
  VLOG(1) << "Adding equivalence class node, with change "
          << chg->GenerateChange();
  AddArcsFromToOtherEquivNodes(ec, ec_node);
  // Return the new EC node
  return ec_node;
}

void FlowGraphManager::AddGraphChange(DIMACSChange* change) {
  if (change->comment().empty()) {
    change->set_comment("AddGraphChange: anonymous caller");
  }
  graph_changes_.push_back(change);
}

void FlowGraphManager::AddMachine(ResourceTopologyNodeDescriptor* root) {
  UpdateResourceTopology(root);
}

void FlowGraphManager::AddArcFromParentToResource(const FlowGraphNode& res_node,
                                                  ResourceID_t parent_res_id,
                                                  vector<FlowGraphArc*>* arcs) {
  // Add arc from parent to us if it doesn't already exist
  FlowGraphNode* parent_node = NodeForResourceID(parent_res_id);
  CHECK_NOTNULL(parent_node);
  FlowGraphArc* arc =
    FindPtrOrNull(parent_node->outgoing_arc_map_, res_node.id_);
  if (!arc) {
    VLOG(2) << "Adding missing arc from parent "
            << parent_node->rd_ptr_->uuid() << "(" << parent_node->id_
            << ") to " << res_node.rd_ptr_->uuid()
            << "("  << res_node.id_ << ").";
    // The arc will have a 0 capacity, but it will be updated
    // by the ConfigureResource methods.
    dimacs_stats_->UpdateStats(ADD_ARC_BETWEEN_RES);
    arcs->push_back(flow_graph_->AddArc(parent_node->id_, res_node.id_));
  }
  InsertIfNotPresent(&resource_to_parent_map_,
                     res_node.resource_id_,
                     parent_res_id);
}

FlowGraphNode* FlowGraphManager::AddNewResourceNode(
    ResourceTopologyNodeDescriptor* rtnd_ptr) {
  ResourceDescriptor* rd_ptr = rtnd_ptr->mutable_resource_desc();
  ResourceID_t res_id = ResourceIDFromString(rd_ptr->uuid());
  vector<FlowGraphArc*> resource_arcs;
  if (rd_ptr->has_friendly_name()) {
    VLOG(2) << "Adding node for resource " << rd_ptr->uuid() << " ("
            << rd_ptr->friendly_name() << ")" << ", type "
            << ENUM_TO_STRING(ResourceDescriptor::ResourceType, rd_ptr->type());
  } else {
    VLOG(2) << "Adding node  for resource " << rd_ptr->uuid() << ", type "
            << ENUM_TO_STRING(ResourceDescriptor::ResourceType, rd_ptr->type());
  }
  FlowGraphNode* res_node = flow_graph_->AddNode();
  SetResourceNodeType(res_node, *rd_ptr);
  InsertIfNotPresent(&resource_to_node_map_, res_id, res_node);
  res_node->resource_id_ = res_id;
  res_node->rd_ptr_ = rd_ptr;
  if (rd_ptr->has_friendly_name()) {
    res_node->comment_ = rd_ptr->friendly_name();
  }
  // Record the parent if we have one
  if (rtnd_ptr->has_parent_id()) {
    AddArcFromParentToResource(*res_node,
                               ResourceIDFromString(rtnd_ptr->parent_id()),
                               &resource_arcs);
  }
  // Add new resource node to the graph changes.
  DIMACSChange *chg = new DIMACSAddNode(*res_node, resource_arcs);
  chg->set_comment("AddResourceNode");
  dimacs_stats_->UpdateStats(ADD_RESOURCE_NODE);
  AddGraphChange(chg);

  if (rd_ptr->type() == ResourceDescriptor::RESOURCE_MACHINE) {
    trace_generator_->AddMachine(*rd_ptr);
    // We call AddMachine here, but do *not* yet create the ECs, since the
    // outgoing arcs from the ECs must know the number of task slots in the
    // machine, which isn't clear until we've recursed further.
    cost_model_->AddMachine(rtnd_ptr);
  }
  return res_node;
}

void FlowGraphManager::AddOrUpdateJobNodes(JobDescriptor* jd) {
  JobID_t job_id = JobIDFromString(jd->uuid());
  // First add an unscheduled aggregator node for this job
  // if none exists already.
  FlowGraphNode* unsched_agg_node = AddOrUpdateJobUnscheduledAgg(job_id);
  // Now add the job's task nodes
  // TODO(malte): This is a simple BFS lashup; maybe we can do better?
  queue<TaskDescriptor*> q;
  q.push(jd->mutable_root_task());
  while (!q.empty()) {
    TaskDescriptor* cur = q.front();
    q.pop();
    // Check if this node has already been added
    uint64_t* tn_ptr = FindOrNull(task_to_nodeid_map_, cur->uid());
    FlowGraphNode* task_node = NULL;
    if (tn_ptr) {
      task_node = flow_graph_->Node(*tn_ptr);
    }
    if (cur->state() == TaskDescriptor::RUNNABLE && !task_node) {
      task_node = AddTaskNode(jd, cur, unsched_agg_node);
      AddTaskEquivClasses(task_node);
    } else if (task_node && cur->state() == TaskDescriptor::RUNNABLE) {
      // We already have the task's nodes, so we need to revisit the ECs to see
      // if any new arcs need to be added
      // TODO(malte): note that this isn't particularly efficient, since we
      // revisit the EC again even if we've already revisited it for another
      // task in the same job or in another. In the future, we might want to
      // track this and avoid doing redundant work.
      vector<EquivClass_t>* equiv_classes =
        cost_model_->GetTaskEquivClasses(task_node->task_id_);
      // If there are no equivalence classes, there's nothing to do
      if (equiv_classes) {
        // Otherwise, revisit each EC and add missing arcs
        for (auto& equiv_class : *equiv_classes) {
          vector<FlowGraphArc*> ec_arcs;
          AddOrUpdateEquivClassArcs(equiv_class, &ec_arcs);
          for (auto& arc : ec_arcs) {
            DIMACSChange* chg = new DIMACSNewArc(*arc);
            chg->set_comment("AddOrUpdateJobNodes: add EC arc");
            dimacs_stats_->UpdateStats(ADD_ARC_TASK_TO_EQUIV_CLASS);
            AddGraphChange(chg);
          }
        }
      }
      delete equiv_classes;
    } else if (cur->state() == TaskDescriptor::RUNNING ||
               cur->state() == TaskDescriptor::ASSIGNED) {
      // The task is already running, so it must have a node already
      //task_node->type_ = FlowNodeType::SCHEDULED_TASK;
    } else if (task_node) {
      VLOG(2) << "Ignoring task " << cur->uid()
              << ", as its node already exists.";
    } else {
      VLOG(2) << "Ignoring task " << cur->uid() << " [" << hex << cur
              << "], which is in state "
              << ENUM_TO_STRING(TaskDescriptor::TaskState, cur->state());
    }
    // Enqueue any existing children of this task
    for (auto& task : *cur->mutable_spawned()) {
      // We do actually need to push tasks even if they are already completed,
      // failed or running, since they may have children eligible for
      // scheduling.
      q.push(&task);
    }
  }
  // Set the excess on the unscheduled node to the difference between the
  // maximum number of running tasks for this job and the number of tasks
  // (F_j - N_j in Quincy terms).
  // TODO(malte): Stub -- this currently allows an unlimited number of tasks per
  // job to be scheduled.
  unsched_agg_node->excess_ = 0;
}

FlowGraphNode* FlowGraphManager::AddOrUpdateJobUnscheduledAgg(JobID_t job_id) {
  uint64_t* unsched_agg_node_id = FindOrNull(job_unsched_to_node_id_, job_id);
  FlowGraphNode* unsched_agg_node;
  if (!unsched_agg_node_id) {
    unsched_agg_node = flow_graph_->AddNode();
    unsched_agg_node->type_ = FlowNodeType::JOB_AGGREGATOR;
    unsched_agg_node->comment_ = "UNSCHED_AGG_for_" + to_string(job_id);
    // ... and connect it directly to the sink
    FlowGraphArc* unsched_agg_to_sink_arc =
      flow_graph_->AddArc(unsched_agg_node, sink_node_);
    unsched_agg_to_sink_arc->cap_upper_bound_ = 0;
    unsched_agg_to_sink_arc->cost_ =
      cost_model_->UnscheduledAggToSinkCost(job_id);
    // Record this for the future in the job <-> node ID lookup table
    CHECK(InsertIfNotPresent(&job_unsched_to_node_id_,
                             job_id,
                             unsched_agg_node->id_));
    dimacs_stats_->UpdateStats(ADD_ARC_FROM_UNSCHED);
    // Add new job unscheduled agg to the graph changes.
    vector<FlowGraphArc*> unsched_arcs;
    unsched_arcs.push_back(unsched_agg_to_sink_arc);
    DIMACSChange *chg = new DIMACSAddNode(*unsched_agg_node, unsched_arcs);
    chg->set_comment("AddOrUpdateJobNodes: unsched_agg");
    dimacs_stats_->UpdateStats(ADD_UNSCHED_JOB_NODE);
    AddGraphChange(chg);
  } else {
    CHECK_NOTNULL(unsched_agg_node_id);
    unsched_agg_node = flow_graph_->Node(*unsched_agg_node_id);
    CHECK_NOTNULL(unsched_agg_node);
  }
  return unsched_agg_node;
}

void FlowGraphManager::AddResourceEquivClasses(FlowGraphNode* res_node) {
  ResourceID_t res_id = res_node->resource_id_;
  vector<EquivClass_t>* equiv_classes =
    cost_model_->GetResourceEquivClasses(res_id);
  // If we have no equivalence classes, there's nothing to do
  if (!equiv_classes)
    return;
  // Otherwise, add the ECs
  VLOG(2) << "Adding resource equiv classes for node " << res_node->id_;
  for (auto& equiv_class : *equiv_classes) {
    FlowGraphNode* ec_node_ptr = FindPtrOrNull(tec_to_node_, equiv_class);
    VLOG(2) << "   EC: " << equiv_class;
    if (ec_node_ptr == NULL) {
      // Node for resource equiv class doesn't yet exist.
      VLOG(2) << "    Adding node EC node for " << equiv_class;
      ec_node_ptr = AddEquivClassNode(equiv_class);
    } else {
      // Node for resource equiv class already exists. Add arc from it.
      // XXX(ionel): We don't add arcs from tasks or other equiv classes
      // to the resource equiv class when a new resource is connected to it.

      FlowGraphArc* ec_arc = FindPtrOrNull(res_node->incoming_arc_map_,
                                           ec_node_ptr->id_);
      if (!ec_arc) {
        ec_arc = flow_graph_->AddArc(ec_node_ptr->id_, res_node->id_);
      }
      pair<Cost_t, int64_t> cost_and_cap =
        cost_model_->EquivClassToResourceNode(equiv_class, res_id);
      if (cost_and_cap.second >= 0) {
        // Use capacity specified by cost model
        ec_arc->cap_upper_bound_ = cost_and_cap.second;
      } else {
        // Cost model did not specify a capacity, so use the maximum
        // TODO(malte): N.B.: this assumes no PU sharing.
        ec_arc->cap_upper_bound_ = CountTaskSlotsBelowResourceNode(res_node);
      }
      ec_arc->cost_ = cost_and_cap.first;
      VLOG(2) << "    Adding arc from EC node " << ec_node_ptr->id_
              << " to " << res_node->id_ << " at cap "
              << ec_arc->cap_upper_bound_ << ", cost " << ec_arc->cost_ << "!";

      DIMACSChange *chg = new DIMACSNewArc(*ec_arc);
      chg->set_comment("AddResourceEquivClasses: from EC to RES");
      dimacs_stats_->UpdateStats(ADD_ARC_EQUIV_CLASS_TO_RES);
      AddGraphChange(chg);
    }
  }
  delete equiv_classes;
}

void FlowGraphManager::AddResourceTopology(
    ResourceTopologyNodeDescriptor* resource_tree) {
  BFSTraverseResourceProtobufTreeReturnRTND(
      resource_tree,
      boost::bind(&FlowGraphManager::AddOrUpdateResourceNode, this, _1));
  BFSTraverseResourceProtobufTreeReturnRTND(
      resource_tree,
      boost::bind(&FlowGraphManager::ConfigureResourceNodeECs, this, _1));
}

void FlowGraphManager::AddOrUpdateResourceNode(
    ResourceTopologyNodeDescriptor* rtnd_ptr) {
  CHECK_NOTNULL(rtnd_ptr);
  ResourceID_t res_id = ResourceIDFromString(rtnd_ptr->resource_desc().uuid());
  FlowGraphNode* res_node = NodeForResourceID(res_id);
  // Add the node if it does not already exist
  if (!res_node) {
    res_node = AddNewResourceNode(rtnd_ptr);
  }
  // Consider different cases: internal (branch) node and leaf node
  if (rtnd_ptr->children_size() > 0) {
    // 1) Node inside the tree with non-zero children (i.e. no leaf node)
    ConfigureResourceBranchNode(*rtnd_ptr, res_node);
  } else if (rtnd_ptr->has_parent_id()) {
    // 2) Leaves of the resource topology; add an arc to the sink node
    ConfigureResourceLeafNode(*rtnd_ptr, res_node);
  }
}

void FlowGraphManager::AddOrUpdateEquivClassArcs(
    EquivClass_t ec,
    vector<FlowGraphArc*>* ec_arcs) {
  // NOTE: We do not create DIMACS changes for new arcs because we can add
  // all the arcs in one go when we create a new node. Otherwise,
  // if we just update an existing node then the code calling this method
  // must generate DIMACS changes.

  FlowGraphNode* ec_node = FindPtrOrNull(tec_to_node_, ec);
  CHECK_NOTNULL(ec_node);
  vector<TaskID_t>* task_pref_arcs =
    cost_model_->GetIncomingEquivClassPrefArcs(ec);
  if (task_pref_arcs) {
    // Add the incoming arcs to the equivalence class node.
    for (auto& task_id : *task_pref_arcs) {
      FlowGraphNode* task_node = NodeForTaskID(task_id);
      // The equivalence class may contain many tasks, and not all of them
      // may have corresponding task nodes in the flow graph yet. This is the
      // case for example when a job with many tasks is added and we process
      // AddEquivClassArcs for the first time. It's fine to simply ignore the
      // not-yet-existing tasks here, because we know that we will come back
      // later and call in here again, with the task present, at which point
      // the correct arc will be added.
      // TODO(malte): think about whether we can do this more elegantly; the
      // above implicit assumption may not always hold (even though it does
      // for now).
      if (!task_node) {
        VLOG(2) << "Skipping addition of arc from task " << task_id << "'s "
                << "node to EC " << ec << " as task not yet present.";
        continue;
      }
      uint64_t arc_cost = cost_model_->TaskToEquivClassAggregator(task_id, ec);
      FlowGraphArc* arc = FindPtrOrNull(task_node->outgoing_arc_map_,
                                        ec_node->id_);
      if (!arc) {
        // We don't have the arc yet, so add it
        VLOG(2) << "Adding arc from task " << task_id << " to EC " << ec;
        arc = flow_graph_->AddArc(task_node->id_, ec_node->id_);
        // XXX(ionel): Increase the capacity if we want to allow for PU sharing.
        arc->cap_upper_bound_ = 1;
        arc->cost_ = arc_cost;
        ec_arcs->push_back(arc);
        dimacs_stats_->UpdateStats(ADD_ARC_TASK_TO_EQUIV_CLASS);
      } else if (arc_cost != arc->cost_) {
        // It already exists, but its cost has changed
        Cost_t old_cost = arc->cost_;
        flow_graph_->ChangeArcCost(arc, arc_cost);
        DIMACSChange *chg = new DIMACSChangeArc(*arc, old_cost);
        chg->set_comment("AddOrUpdateEquivClassArcs/incoming");
        dimacs_stats_->UpdateStats(CHG_ARC_TASK_TO_EQUIV_CLASS);
        AddGraphChange(chg);
      }
    }
    delete task_pref_arcs;
  }
  vector<ResourceID_t>* res_pref_arcs =
    cost_model_->GetOutgoingEquivClassPrefArcs(ec);
  if (res_pref_arcs) {
    // Add the outgoing arcs from the equivalence class node.
    for (auto& res_id : *res_pref_arcs) {
      FlowGraphNode* rn = NodeForResourceID(res_id);
      CHECK_NOTNULL(rn);
      pair<Cost_t, int64_t> cost_and_cap =
        cost_model_->EquivClassToResourceNode(ec, res_id);
      Cost_t arc_cost = cost_and_cap.first;
      FlowGraphArc* arc = FindPtrOrNull(ec_node->outgoing_arc_map_,
                                        rn->id_);
      if (!arc) {
        // We don't have the arc yet, so add it
        arc = flow_graph_->AddArc(ec_node->id_, rn->id_);
        if (cost_and_cap.second >= 0) {
          // Use the capacity from the cost model
          arc->cap_upper_bound_ = cost_and_cap.second;
        } else {
          // Cost model did not give us a capacity, so we use the maximum
          // TODO(malte): N.B.: this assumes no PU sharing.
          arc->cap_upper_bound_ = CountTaskSlotsBelowResourceNode(rn);
        }
        arc->cost_ = arc_cost;
        VLOG(2) << "    adding arc from EC node " << ec_node->id_
                << " to " << rn->id_ << " at cap "
                << arc->cap_upper_bound_ << ", cost " << arc->cost_ << "!";
        ec_arcs->push_back(arc);
        dimacs_stats_->UpdateStats(ADD_ARC_EQUIV_CLASS_TO_RES);
      } else if (static_cast<uint64_t>(arc_cost) != arc->cost_) {
        // It already exists, but its cost has changed
        VLOG(1) << "Updating cost on EC -> resource arc from " << ec
                << " to " << arc->dst_node_->resource_id_ << " from "
                << arc->cost_ << " to " << arc_cost;
        Cost_t old_cost = arc->cost_;
        flow_graph_->ChangeArcCost(arc, arc_cost);
        DIMACSChange *chg = new DIMACSChangeArc(*arc, old_cost);
        chg->set_comment("AddOrUpdateEquivClassArcs/outgoing");
        dimacs_stats_->UpdateStats(CHG_ARC_EQUIV_CLASS_TO_RES);
        AddGraphChange(chg);
      }
    }
  }
  RemoveInvalidPreferenceArcs(*ec_node, *res_pref_arcs);
  // Finally, throw the preference arc vector away
  delete res_pref_arcs;
}

void FlowGraphManager::AddSpecialNodes() {
  // Sink node
  sink_node_ = flow_graph_->AddNode();
  sink_node_->type_ = FlowNodeType::SINK;
  sink_node_->comment_ = "SINK";
  dimacs_stats_->UpdateStats(ADD_SINK_NODE);
  AddGraphChange(new DIMACSAddNode(*sink_node_, vector<FlowGraphArc*>()));
  // N.B.: we do NOT create a cluster aggregator node here, since
  // not all cost models use one. Instead, cost models add it as a special
  // equivalence class.
}

void FlowGraphManager::AddTaskEquivClasses(FlowGraphNode* task_node) {
  vector<EquivClass_t>* equiv_classes =
    cost_model_->GetTaskEquivClasses(task_node->task_id_);
  // If there are no equivalence classes, there's nothing to do
  if (!equiv_classes)
    return;
  // Otherwise, add the ECs
  for (auto& equiv_class : *equiv_classes) {
    FlowGraphNode* ec_node_ptr = FindPtrOrNull(tec_to_node_, equiv_class);
    if (ec_node_ptr == NULL) {
      // Node for task equiv class doesn't yet exist.
      ec_node_ptr = AddEquivClassNode(equiv_class);
    }
    // Node for task equiv class now exists for sure. Add arc to it if we
    // don't already have one: we might, as the act of adding the EC above
    // mav already have created the arc, if it is returned by
    // AddEquivClassNode's cost_model_->GetIncomingEquivClassPrefArcs().
    if (!FlowGraph::GetArc(task_node, ec_node_ptr)) {
      VLOG(2) << "AddTaskEquivClasses adding arc from task " << task_node->id_
              << " to EC " << ec_node_ptr->id_;
      FlowGraphArc* ec_arc =
        flow_graph_->AddArc(task_node->id_, ec_node_ptr->id_);
      ec_arc->cap_upper_bound_ = 1;
      ec_arc->cost_ =
        cost_model_->TaskToEquivClassAggregator(task_node->task_id_,
                                                equiv_class);
      DIMACSChange *chg = new DIMACSNewArc(*ec_arc);
      dimacs_stats_->UpdateStats(ADD_ARC_TASK_TO_EQUIV_CLASS);
      chg->set_comment("AddTaskEquivClasses");
      AddGraphChange(chg);
    }
    // TODO(ionel): We don't add new arcs from the equivalence class to
    // resource nodes or other ECs when we add a new arc from a task to the
    // equiv class.
    // We may want to do add some in the future.
  }
  delete equiv_classes;
}

FlowGraphNode* FlowGraphManager::AddTaskNode(JobDescriptor* jd_ptr,
                                             TaskDescriptor* td_ptr,
                                             FlowGraphNode* unsched_agg_node) {
  trace_generator_->TaskSubmitted(jd_ptr, td_ptr);
  vector<FlowGraphArc*> task_arcs;
  FlowGraphNode* task_node = flow_graph_->AddNode();
  task_node->type_ = FlowNodeType::UNSCHEDULED_TASK;
  // Add the current task's node
  task_node->excess_ = 1;
  task_node->task_id_ = td_ptr->uid();  // set task ID in node
  task_node->job_id_ = JobIDFromString(jd_ptr->uuid());
  sink_node_->excess_--;
  task_nodes_.insert(task_node->id_);
  // Insert a record for the node representing this task's ID
  InsertIfNotPresent(&task_to_nodeid_map_, td_ptr->uid(), task_node->id_);
  // Log info
  VLOG(2) << "Adding edges for task " << td_ptr->uid() << "'s node ("
          << task_node->id_ << "); task state is " << td_ptr->state();
  // Arcs for this node
  AddArcsForTask(task_node, unsched_agg_node, &task_arcs);
  // Add the new task node to the graph changes
  DIMACSChange *chg = new DIMACSAddNode(*task_node, task_arcs);
  chg->set_comment("AddOrUpdateJobNodes: task node");
  dimacs_stats_->UpdateStats(ADD_TASK_NODE);
  AddGraphChange(chg);
  return task_node;
}

uint64_t FlowGraphManager::CapacityBetweenECNodes(const FlowGraphNode& src,
                                                  const FlowGraphNode& dst) {
  // Compute sum of incoming capacities at src
  uint64_t in_sum = 0;
  for (auto& incoming_arc : src.incoming_arc_map_) {
    in_sum += incoming_arc.second->cap_upper_bound_;
  }
  // Compute sum of incoming capacities at src
  uint64_t out_sum = 0;
  for (auto& outgoing_arc : dst.outgoing_arc_map_) {
    out_sum += outgoing_arc.second->cap_upper_bound_;
  }
  return max(in_sum, out_sum);
}

bool FlowGraphManager::CheckNodeType(uint64_t node, FlowNodeType type) {
  FlowNodeType node_type = flow_graph_->Node(node)->type_;
  return (node_type == type);
}

void FlowGraphManager::ComputeTopologyStatistics(
    FlowGraphNode* node,
    boost::function<void(FlowGraphNode*)> prepare,
    boost::function<FlowGraphNode*(FlowGraphNode*, FlowGraphNode*)> gather,
    boost::function<FlowGraphNode*(FlowGraphNode*, FlowGraphNode*)> update) {
  // XXX(ionel): The function only works correctly as long as the topology is a
  // tree. If the topology is a DAG then it does not work correctly! It does
  // not work in the DAG case because the function implements BFS. Hence,
  // we may pop a not of the queue and propagate its statistics via its incoming
  // arcs before we've received all the statistics at the node.
  queue<FlowGraphNode*> to_visit;
  // We maintain a value that is used to mark visited nodes. Before each
  // visit we increment the mark to make sure that nodes visited in previous
  // traversal are not going to be treated as marked. By using the mark
  // variable we avoid having to reset the visited state of each node before
  // of a traversal.
  cur_traversal_counter_++;
  to_visit.push(node);
  node->visited_ = cur_traversal_counter_;
  while (!to_visit.empty()) {
    FlowGraphNode* cur_node = to_visit.front();
    to_visit.pop();
    for (auto& incoming_arc : cur_node->incoming_arc_map_) {
      if (incoming_arc.second->src_node_->visited_ != cur_traversal_counter_) {
        if (prepare) {
          prepare(incoming_arc.second->src_node_);
        }
        to_visit.push(incoming_arc.second->src_node_);
        incoming_arc.second->src_node_->visited_ = cur_traversal_counter_;
      }
      incoming_arc.second->src_node_ =
        gather(incoming_arc.second->src_node_, cur_node);
      incoming_arc.second->src_node_ =
        update(incoming_arc.second->src_node_, cur_node);
    }
  }
}

void FlowGraphManager::ConfigureResourceBranchNode(
    const ResourceTopologyNodeDescriptor& rtnd, FlowGraphNode* new_node) {
  // Add internal arc from parent
  if (rtnd.has_parent_id()) {
    FlowGraphNode* parent_node = NodeForResourceID(
        ResourceIDFromString(rtnd.parent_id()));
    CHECK(parent_node != NULL) << "Could not find parent node with ID "
                               << rtnd.parent_id();
    // Find the arc from parent node (which should have been added before)
    FlowGraphArc* arc = FindPtrOrNull(parent_node->outgoing_arc_map_,
                                      new_node->id_);
    CHECK_NOTNULL(arc);
    // Set initial capacity to 0; this will be updated as leaves are added
    // below this node!
    arc->cap_upper_bound_ = 0;
    if (!new_node->rd_ptr_ || !parent_node->rd_ptr_) {
      arc->cost_ = 0;
    } else {
      arc->cost_ =
        cost_model_->ResourceNodeToResourceNodeCost(
            *parent_node->rd_ptr_, *new_node->rd_ptr_);
    }
    // XXX(ionel): The new arc has a capacity of 0. This is treated as an arc
    // removal by the DIMACS extended. We add the arc to the graph changes later
    // when we change the capacity.
    // graph_changes_.push_back(new DIMACSChangeArc(*arc));
  } else if (new_node->type_ != FlowNodeType::COORDINATOR &&
             rtnd.resource_desc().type() !=
               ResourceDescriptor::RESOURCE_COORDINATOR &&
             rtnd.resource_desc().type() !=
               ResourceDescriptor::RESOURCE_MACHINE &&
             rtnd.resource_desc().type() !=
               ResourceDescriptor::RESOURCE_PU) {
    // Having no parent is only okay if we're the root node
    // TODO(malte): we currently override this for the RESOURCE_MACHINE
    // and RESOURCE_PU cases to avoid unit tests (which have no coordinator
    // node) failing.
    // Come back to fix.
    LOG(FATAL) << "Found child without parent_id set! This will lead to an "
               << "inconsistent flow graph! child ID: "
               << rtnd.resource_desc().uuid();
  }
}

void FlowGraphManager::ConfigureResourceLeafNode(
    const ResourceTopologyNodeDescriptor& rtnd, FlowGraphNode* new_node) {
  VLOG(2) << "Considering node " << rtnd.resource_desc().uuid()
          << ", which has parent "
          << (rtnd.has_parent_id() ? rtnd.parent_id() : "NONE");
  VLOG(2) << "Adding arc from leaf resource " << rtnd.resource_desc().uuid()
          << " to sink node.";
  if (rtnd.resource_desc().type() != ResourceDescriptor::RESOURCE_PU)
    LOG(FATAL) << "Leaf resource " << rtnd.resource_desc().uuid()
               << " is not a PU! This may yield an unschedulable flow!";
  FlowGraphNode* cur_node = NodeForResourceID(
      ResourceIDFromString(rtnd.resource_desc().uuid()));
  CHECK(cur_node != NULL) << "Could not find leaf node with ID "
                          << rtnd.resource_desc().uuid();
  cur_node->type_ = FlowNodeType::PU;
  FlowGraphArc* arc = flow_graph_->AddArc(cur_node->id_, sink_node_->id_);
  arc->cap_upper_bound_ = 1;
  // TODO(malte): change this if support time-sharing
  arc->cost_ =
      cost_model_->LeafResourceNodeToSinkCost(cur_node->resource_id_);
  leaf_nodes_.insert(cur_node->id_);
  leaf_res_ids_->insert(cur_node->resource_id_);

  DIMACSChange *chg = new DIMACSNewArc(*arc);
  chg->set_comment("ConfigureResourceLeafNode");
  dimacs_stats_->UpdateStats(ADD_ARC_BETWEEN_RES);
  AddGraphChange(chg);
  // Add flow capacity to parent nodes until we hit the root node
  FlowGraphNode* parent = cur_node;
  ResourceID_t* parent_id;
  while ((parent_id = FindOrNull(resource_to_parent_map_,
                                 parent->resource_id_)) != NULL) {
    uint64_t cur_id = parent->id_;
    parent = NodeForResourceID(*parent_id);
    FlowGraphArc* arc = FindPtrOrNull(parent->outgoing_arc_map_, cur_id);
    CHECK_NOTNULL(arc);
    VLOG(2) << "Adding capacity on edge from " << *parent_id << " ("
            << parent->id_ << ") to " << cur_id << " ("
            << arc->cap_upper_bound_ << " -> "
            << arc->cap_upper_bound_ + 1 << ")";
    arc->cap_upper_bound_ += 1;
    // TODO(malte): we don't set the cost here, but probably should. However,
    // care needs to be taken, because ConfigureResourceLeafNode is called
    // before cost model state is set up, so not all information may be
    // available.

    DIMACSChange *chg = new DIMACSChangeArc(*arc, arc->cost_);
    chg->set_comment("ConfigureResourceLeafNode");
    dimacs_stats_->UpdateStats(CHG_ARC_BETWEEN_RES);
    AddGraphChange(chg);
  }
}

void FlowGraphManager::ConfigureResourceNodeECs(
    ResourceTopologyNodeDescriptor* rtnd) {
  FlowGraphNode* node = NodeForResourceID(
      ResourceIDFromString(rtnd->resource_desc().uuid()));
  // We only add the EC here so that we know the correct number of task slots
  // below it.
  if (rtnd->resource_desc().type() == ResourceDescriptor::RESOURCE_MACHINE) {
    AddResourceEquivClasses(node);
  }
}

uint32_t FlowGraphManager::CountTaskSlotsBelowResourceNode(
    FlowGraphNode* node) {
  uint32_t task_slot_count = 0;
  // Iterate over the children of this node to find leaves and aggregate
  // their capacities to the sink.
  // Assumes acyclicity, which we have in any reasonable flow graph
  queue<FlowGraphNode*> q;
  q.push(node);
  // BFS over subtree
  while (!q.empty()) {
    FlowGraphNode* n = q.front();
    q.pop();
    for (unordered_map<uint64_t, FlowGraphArc*>::iterator it =
         n->outgoing_arc_map_.begin();
         it != n->outgoing_arc_map_.end();
         ++it) {
      if (it->second->dst_node_->type_ == FlowNodeType::SINK) {
        task_slot_count += it->second->cap_upper_bound_;
      } else {
        q.push(it->second->dst_node_);
      }
    }
  }
  return task_slot_count;
}

void FlowGraphManager::DeleteResourceNode(FlowGraphNode* res_node,
                                          const char *comment) {
  ResourceID_t res_id = res_node->resource_id_;
  ResourceID_t res_id_tmp = res_id;
  resource_to_parent_map_.erase(res_id);
  leaf_nodes_.erase(res_node->id_);
  // This erase is going to delete the res_id. That's why we use
  // res_id_tmp from now onwards.
  leaf_res_ids_->erase(res_id);
  resource_to_node_map_.erase(res_id_tmp);
  VLOG(2) << "Deleting node " << res_node->id_;
  DIMACSChange *chg = new DIMACSRemoveNode(*res_node);
  chg->set_comment(comment);
  dimacs_stats_->UpdateStats(DEL_RESOURCE_NODE);
  AddGraphChange(chg);
  flow_graph_->DeleteNode(res_node);
  vector<EquivClass_t>* equiv_classes =
    cost_model_->GetResourceEquivClasses(res_id_tmp);
  // If there are no equivalence classes, we're done
  if (!equiv_classes)
    return;
  // Otherwise, delete the equivalence class nodes if required
  for (auto& equiv_class : *equiv_classes) {
    DeleteOrUpdateIncomingEquivNode(equiv_class, comment);
  }
  delete equiv_classes;
}

void FlowGraphManager::DeleteOrUpdateIncomingEquivNode(EquivClass_t task_equiv,
                                                       const char *comment) {
  FlowGraphNode* equiv_node_ptr = FindPtrOrNull(tec_to_node_, task_equiv);
  if (equiv_node_ptr == NULL) {
    // Equiv class node can be NULL because all its tasks are running
    // and are directly connected to resource nodes.
    return;
  }
  if (equiv_node_ptr->outgoing_arc_map_.size() == 0) {
    VLOG(2) << "Deleting resource_equiv class: " << task_equiv;
    // The equiv class doesn't have any incoming arcs from tasks.
    // We can remove the node.
    tec_to_node_.erase(task_equiv);
    DIMACSChange *chg = new DIMACSRemoveNode(*equiv_node_ptr);
    dimacs_stats_->UpdateStats(DEL_EQUIV_CLASS_NODE);
    chg->set_comment(comment);
    AddGraphChange(chg);
    flow_graph_->DeleteNode(equiv_node_ptr);
  } else {
    // TODO(ionel): We may want to reduce the number of outgoing
    // arcs from the equiv class to cores. However, this is not
    // mandatory.
  }
}

void FlowGraphManager::DeleteOrUpdateOutgoingEquivNode(EquivClass_t task_equiv,
                                                       const char *comment) {
  FlowGraphNode* equiv_node_ptr = FindPtrOrNull(tec_to_node_, task_equiv);
  if (equiv_node_ptr == NULL) {
    // Equiv class node can be NULL because all its tasks are running
    // and are directly connected to resource nodes.
    return;
  }
  if (equiv_node_ptr->incoming_arc_map_.size() == 0) {
    VLOG(2) << "Deleting task_equiv class: " << task_equiv;
    // The equiv class doesn't have any incoming arcs from tasks.
    // We can remove the node.
    tec_to_node_.erase(task_equiv);
    DIMACSChange *chg = new DIMACSRemoveNode(*equiv_node_ptr);
    chg->set_comment(comment);
    dimacs_stats_->UpdateStats(DEL_EQUIV_CLASS_NODE);
    AddGraphChange(chg);
    flow_graph_->DeleteNode(equiv_node_ptr);
  } else {
    // TODO(ionel): We may want to reduce the number of outgoing
    // arcs from the equiv class to cores. However, this is not
    // mandatory.
  }
}

uint64_t FlowGraphManager::DeleteTaskNode(TaskID_t task_id,
                                          const char *comment) {
  uint64_t* node_id = FindOrNull(task_to_nodeid_map_, task_id);
  CHECK_NOTNULL(node_id);
  uint64_t task_node_id = *node_id;
  FlowGraphNode* node = flow_graph_->Node(task_node_id);
  // Increase the sink's excess and set this node's excess to zero
  node->excess_ = 0;
  sink_node_->excess_++;
  // Find the unscheduled node for this job and decrement its outgoing capacity
  // TODO(malte): this is only relevant if we support preemption; otherwise the
  // capcacity will already have been deducted (as part of PinTaskToNode,
  // currently).
  // Then remove node meta-data
  VLOG(2) << "Deleting task node with id " << node->id_ << ", task id "
          << node->task_id_;
  CHECK_EQ(task_nodes_.erase(node->id_), 1);
  CHECK_EQ(task_to_nodeid_map_.erase(task_id), 1);
  // Then remove the node itself. This needs to happen first, so that the arc
  // counts for ECs are correct.
  DIMACSChange *chg = new DIMACSRemoveNode(*node);
  chg->set_comment(comment);
  dimacs_stats_->UpdateStats(DEL_TASK_NODE);
  AddGraphChange(chg);
  flow_graph_->DeleteNode(node);
  vector<EquivClass_t>* equiv_classes =
    cost_model_->GetTaskEquivClasses(task_id);
  // If there are no ECs, we're done
  if (!equiv_classes)
    return task_node_id;
  // Otherwise, delete the EC aggregators if necessary
  for (auto& equiv_class : *equiv_classes) {
    DeleteOrUpdateOutgoingEquivNode(equiv_class, comment);
  }
  delete equiv_classes;
  return task_node_id;
}

void FlowGraphManager::JobCompleted(JobID_t job_id) {
  uint64_t* unsched_node_id = FindOrNull(job_unsched_to_node_id_, job_id);
  CHECK_NOTNULL(unsched_node_id);
  FlowGraphNode* node = flow_graph_->Node(*unsched_node_id);
  CHECK_EQ(node->incoming_arc_map_.size(), 0);
  job_unsched_to_node_id_.erase(job_id);
  DIMACSChange *chg = new DIMACSRemoveNode(*node);
  chg->set_comment("JobCompleted: unsched");
  dimacs_stats_->UpdateStats(DEL_UNSCHED_JOB_NODE);
  AddGraphChange(chg);
  flow_graph_->DeleteNode(node);
}

FlowGraphNode* FlowGraphManager::NodeForResourceID(const ResourceID_t& res_id) {
  return FindPtrOrNull(resource_to_node_map_, res_id);
}

FlowGraphNode* FlowGraphManager::NodeForTaskID(TaskID_t task_id) {
  uint64_t* id = FindOrNull(task_to_nodeid_map_, task_id);
  // Returns NULL if task unknown
  if (!id)
    return NULL;
  VLOG(2) << "Task " << task_id << " is represented by node " << *id;
  return flow_graph_->Node(*id);
}

void FlowGraphManager::PinTaskToNode(FlowGraphNode* task_node,
                                     FlowGraphNode* res_node) {
  bool added_running_arc = false;
  // Remove all arcs apart from the task -> resource mapping;
  // note that this effectively disables preemption!
  for (unordered_map<uint64_t, FlowGraphArc*>::iterator it =
         task_node->outgoing_arc_map_.begin();
       it != task_node->outgoing_arc_map_.end(); ) {
    FlowGraphArc* arc = it->second;
    ++it;
    if (arc->dst_node_->id_ == res_node->id_ && !added_running_arc) {
      // This preference arc connects the same nodes as the running arc. Hence,
      // we just transform it into the running arc.
      // TODO(ionel): This doesn't allow preemption. Set the
      // cap_lower bound to 0 and the cost appropriately to allow preemption.
      flow_graph_->ChangeArc(arc, 1, 1, 0);
      arc->type_ = RUNNING;
      DIMACSChange *chg = new DIMACSChangeArc(*arc, arc->cost_);
      chg->set_comment("PinTaskToNode transform to running arc");
      dimacs_stats_->UpdateStats(CHG_ARC_PIN_TASK);
      AddGraphChange(chg);
      added_running_arc = true;
    } else {
      VLOG(2) << "Deleting arc from " << arc->src_ << " to " << arc->dst_;
      arc->cap_lower_bound_ = 0;
      arc->cap_upper_bound_ = 0;
      DIMACSChange *chg = new DIMACSChangeArc(*arc, arc->cost_);
      chg->set_comment("PinTaskToNode delete arc");
      dimacs_stats_->UpdateStats(DEL_ARC_PIN_TASK);
      AddGraphChange(chg);
      flow_graph_->DeleteArc(arc);
    }
  }
  // Remove this task's potential flow from the per-job unscheduled
  // aggregator's outgoing edge
  UpdateUnscheduledAggToSinkCapacity(task_node->job_id_, -1);
  if (!added_running_arc) {
    // Re-add a single arc from the task to the resource node
    FlowGraphArc* new_arc = flow_graph_->AddArc(task_node, res_node);
    new_arc->cap_lower_bound_ = 1;
    new_arc->cap_upper_bound_ = 1;
    new_arc->type_ = RUNNING;
    // TODO(ionel): This doesn't allow preemption. Set the
    // cap_lower bound to 0 and the cost appropriately to allow
    // preemption.
    new_arc->cost_ = 0;
    DIMACSChange *chg = new DIMACSNewArc(*new_arc);
    chg->set_comment("PinTaskToNode add running arc");
    dimacs_stats_->UpdateStats(ADD_ARC_PIN_TASK);
    AddGraphChange(chg);
  }
}

void FlowGraphManager::RemoveInvalidPreferenceArcs(
    const FlowGraphNode& ec_node, const vector<ResourceID_t>& res_pref_arcs) {
  // Check if we need to remove any arcs that are no longer in the set
  unordered_set<ResourceID_t, boost::hash<boost::uuids::uuid>> res_preferences(
      res_pref_arcs.begin(),
      res_pref_arcs.end());
  unordered_set<FlowGraphArc*> to_delete;
  for (auto& dst_arc : ec_node.outgoing_arc_map_) {
    ResourceID_t target_rid = dst_arc.second->dst_node_->resource_id_;
    if (res_preferences.find(target_rid) == res_preferences.end()) {
      // We need to remove this arc.
      to_delete.insert(dst_arc.second);
      LOG(INFO) << "Deleting no-longer-current arc from EC " << ec_node.ec_id_
                << " to resource " << target_rid;
    }
  }
  for (auto& arc : to_delete) {
    arc->cap_lower_bound_ = 0;
    arc->cap_upper_bound_ = 0;
    DIMACSChange *chg = new DIMACSChangeArc(*arc, arc->cost_);
    dimacs_stats_->UpdateStats(DEL_ARC_EQUIV_CLASS_TO_RES);
    chg->set_comment("AddOrUpdateEquivClassArcs/outgoing");
    AddGraphChange(chg);
    flow_graph_->DeleteArc(arc);
  }
}

void FlowGraphManager::RemoveMachine(const ResourceDescriptor& rd,
                                     set<uint64_t>* pus_removed) {
  trace_generator_->RemoveMachine(rd);
  ResourceID_t res_id = ResourceIDFromString(rd.uuid());
  FlowGraphNode* res_node = FindPtrOrNull(resource_to_node_map_, res_id);
  CHECK_NOTNULL(res_node);
  RemoveMachineSubTree(res_node, pus_removed);
  cost_model_->RemoveMachine(res_id);
}

void FlowGraphManager::RemoveMachineSubTree(FlowGraphNode* res_node,
                                            set<uint64_t>* pus_removed) {
  while (true) {
    unordered_map<uint64_t, FlowGraphArc*>::iterator
      it = res_node->outgoing_arc_map_.begin();
    if (it == res_node->outgoing_arc_map_.end()) {
      break;
    }
    if (it->second->dst_node_->resource_id_.is_nil()) {
      // The node is not a resource node. We will just delete the arc to it.
      // We don't need to generate a DIMACS change because we'll later delete
      // the node.
      flow_graph_->DeleteArc(it->second);
      continue;
    }
    if (it->second->dst_node_->type_ == FlowNodeType::PU ||
        it->second->dst_node_->type_ == FlowNodeType::MACHINE ||
        it->second->dst_node_->type_ == FlowNodeType::UNKNOWN) {
      RemoveMachineSubTree(it->second->dst_node_, pus_removed);
    } else {
      // The node is not a machine related node. We will just delete the arc
      // to it. We don't need to generate a DIMACS change because we'll later
      // delete the node.
      flow_graph_->DeleteArc(it->second);
    }
  }
  if (res_node->type_ == FlowNodeType::PU) {
    pus_removed->insert(res_node->id_);
  }
  // We've deleted all its children. Now we can delete the node itself.
  DeleteResourceNode(res_node, "RemoveMachineSubTree");
}

void FlowGraphManager::SetResourceNodeType(FlowGraphNode* res_node,
                                           const ResourceDescriptor& rd) {
  if (rd.type() == ResourceDescriptor::RESOURCE_PU) {
    res_node->type_ = FlowNodeType::PU;
  } else if (rd.type() == ResourceDescriptor::RESOURCE_MACHINE) {
    res_node->type_ = FlowNodeType::MACHINE;
  } else if (rd.type() == ResourceDescriptor::RESOURCE_COORDINATOR) {
    res_node->type_ = FlowNodeType::COORDINATOR;
  } else {
    res_node->type_ = FlowNodeType::UNKNOWN;
  }
}

uint64_t FlowGraphManager::TaskCompleted(TaskID_t tid) {
  trace_generator_->TaskCompleted(tid);
  uint64_t task_node_id = DeleteTaskNode(tid, "TaskCompleted");
  cost_model_->RemoveTask(tid);
  return task_node_id;
}

void FlowGraphManager::TaskEvicted(TaskID_t tid, ResourceID_t res_id) {
  trace_generator_->TaskEvicted(tid);
  FlowGraphNode* task_node = NodeForTaskID(tid);
  CHECK_NOTNULL(task_node);
  task_node->type_ = FlowNodeType::UNSCHEDULED_TASK;
  UpdateArcsForEvictedTask(tid, res_id);
  // We do not have to remove the task from the cost model because
  // the task will still exist in the flow graph at the end of
  // UpdateArcsForEvictedTask.
}

void FlowGraphManager::TaskFailed(TaskID_t tid) {
  trace_generator_->TaskFailed(tid);
  DeleteTaskNode(tid, "TaskFailed");
  cost_model_->RemoveTask(tid);
}

void FlowGraphManager::TaskKilled(TaskID_t tid) {
  trace_generator_->TaskKilled(tid);
  DeleteTaskNode(tid, "TaskKilled");
  cost_model_->RemoveTask(tid);
}

void FlowGraphManager::TaskMigrated(TaskID_t tid,
                                    ResourceID_t old_res_id,
                                    ResourceID_t new_res_id) {
  TaskEvicted(tid, old_res_id);
  TaskScheduled(tid, new_res_id);
}

void FlowGraphManager::TaskScheduled(TaskID_t tid, ResourceID_t res_id) {
  trace_generator_->TaskScheduled(tid, res_id);
  // Mark the task as scheduled
  FlowGraphNode* node = NodeForTaskID(tid);
  CHECK_NOTNULL(node);
  node->type_ = FlowNodeType::SCHEDULED_TASK;
  // After the task is bound, we now remove all of its edges into the flow
  // graph apart from the bound resource.
  // N.B.: This disables preemption and migration, unless FLAGS_preemption
  // is set!
  UpdateArcsForBoundTask(tid, res_id);
}

FlowGraphNode* FlowGraphManager::UnscheduledAggregatorForJobID(JobID_t job_id) {
  uint64_t* unsched_agg_node_id = FindOrNull(job_unsched_to_node_id_, job_id);
  if (unsched_agg_node_id == NULL) {
    LOG(WARNING) << "Job " << job_id << " does not have an unscheduled "
                 << "aggregator node";
    return NULL;
  }
  return flow_graph_->Node(*unsched_agg_node_id);
}

void FlowGraphManager::UpdateArcsForBoundTask(TaskID_t tid,
                                              ResourceID_t res_id) {
  FlowGraphNode* task_node = NodeForTaskID(tid);
  FlowGraphNode* assigned_res_node = NodeForResourceID(res_id);
  CHECK_NOTNULL(task_node);
  CHECK_NOTNULL(assigned_res_node);
  if (!FLAGS_preemption) {
    // After the task is bound, we now remove all of its edges into the flow
    // graph apart from the bound resource.
    // N.B.: This disables preemption and migration!
    VLOG(2) << "Disabling preemption for " << tid;
    // Disable preemption
    PinTaskToNode(task_node, assigned_res_node);
  }
}

void FlowGraphManager::UpdateArcsForEvictedTask(TaskID_t task_id,
                                                ResourceID_t res_id) {
  FlowGraphNode* task_node = NodeForTaskID(task_id);
  CHECK_NOTNULL(task_node);
  if (!FLAGS_preemption) {
    // Delete outgoing arcs for running task.
    for (unordered_map<uint64_t, FlowGraphArc*>::iterator it =
           task_node->outgoing_arc_map_.begin();
         it != task_node->outgoing_arc_map_.end();) {
      FlowGraphArc* arc = it->second;
      ++it;
      arc->cap_lower_bound_ = 0;
      arc->cap_upper_bound_ = 0;
      DIMACSChange* chg = new DIMACSChangeArc(*arc, arc->cost_);
      chg->set_comment("UpdateArcsForEvictedTasks delete running arc");
      dimacs_stats_->UpdateStats(DEL_ARC_EVICTED_TASK);
      AddGraphChange(chg);
      flow_graph_->DeleteArc(arc);
    }
    // Add back arcs to equiv class node, unscheduled agg and to
    // resource topology agg.
    vector<FlowGraphArc*> *task_arcs = new vector<FlowGraphArc*>();
    uint64_t* unsched_agg_node_id = FindOrNull(job_unsched_to_node_id_,
                                               task_node->job_id_);
    FlowGraphNode* unsched_agg_node_ptr =
      flow_graph_->Node(*unsched_agg_node_id);
    CHECK_NOTNULL(unsched_agg_node_ptr);

    AddArcsForTask(task_node, unsched_agg_node_ptr, task_arcs);
    for (auto& task_arc : *task_arcs) {
      DIMACSChange *chg = new DIMACSNewArc(*task_arc);
      chg->set_comment("UpdateArcsForEvictedTask");
      dimacs_stats_->UpdateStats(CHG_ARC_EVICTED_TASK);
      AddGraphChange(chg);
    }
    delete task_arcs;

    AddTaskEquivClasses(task_node);

    // Add this task's potential flow from the per-job unscheduled
    // aggregator's outgoing edge
    UpdateUnscheduledAggToSinkCapacity(task_node->job_id_, 1);
  }
}

void FlowGraphManager::UpdateResourceNode(
    ResourceTopologyNodeDescriptor* rtnd_ptr) {
  CHECK_NOTNULL(rtnd_ptr);
  const ResourceTopologyNodeDescriptor& rtnd = *rtnd_ptr;
  ResourceID_t res_id = ResourceIDFromString(rtnd.resource_desc().uuid());
  // First of all, check if this node already exists in our resource topology
  FlowGraphNode* found_node = FindPtrOrNull(resource_to_node_map_, res_id);
  if (found_node) {
    // Check if its parent is identical
    if (rtnd.has_parent_id()) {
      ResourceID_t* old_parent_id = FindOrNull(resource_to_parent_map_, res_id);
      ResourceID_t new_parent_id = ResourceIDFromString(rtnd.parent_id());
      // We didn't have a parent or the parent we had was different, so we need
      // to move the node
      if (!old_parent_id || *old_parent_id != new_parent_id) {
        // If not, we need to move it to the new parent
        InsertOrUpdate(&resource_to_parent_map_, res_id, new_parent_id);
        // Remove arc corresponding to the old parent/child relationship
        FlowGraphNode* new_parent_node =
            FindPtrOrNull(resource_to_node_map_, new_parent_id);
        CHECK_NOTNULL(new_parent_node);
        LOG(FATAL) << "Moving resources to new parents not supported yet";
      }
      // Parent is the same as before (and not NULL)
      if (old_parent_id) {
        FlowGraphNode* old_parent_node =
            FindPtrOrNull(resource_to_node_map_, *old_parent_id);
        CHECK_NOTNULL(old_parent_node);
        // TODO(malte): Is there anything we need to do here?
      }
    }
    // Check if any children need adding
    for (RepeatedPtrField<ResourceTopologyNodeDescriptor>::pointer_iterator
         child_iter = rtnd_ptr->mutable_children()->pointer_begin();
         child_iter != rtnd_ptr->mutable_children()->pointer_end();
         ++child_iter) {
      ResourceID_t child_res_id =
          ResourceIDFromString((*child_iter)->resource_desc().uuid());
      FlowGraphNode* child_node =
          FindPtrOrNull(resource_to_node_map_, child_res_id);
      if (!child_node)
        AddResourceTopology(*child_iter);
    }
  } else {
    // It does not already exist, so add it.
    VLOG(1) << "Adding new resource " << res_id << " to flow graph.";
    // N.B.: We need to ensure we hook in at the right place here by setting the
    // parent ID appropriately if it is not already.
    AddOrUpdateResourceNode(rtnd_ptr);
  }
}

void FlowGraphManager::UpdateResourceTopology(
    ResourceTopologyNodeDescriptor* resource_tree) {
  // N.B.: This only considers ADDITION of resources currently; if resources
  // are removed from the topology (e.g. due to a failure), they won't
  // disappear via this method.
  BFSTraverseResourceProtobufTreeReturnRTND(
      resource_tree,
      boost::bind(&FlowGraphManager::UpdateResourceNode, this, _1));
  VLOG(2) << "Updated resource topology in flow scheduler.";
}

void FlowGraphManager::ResetChanges() {
  for (vector<DIMACSChange*>::iterator it = graph_changes_.begin();
       it != graph_changes_.end(); ) {
    vector<DIMACSChange*>::iterator it_tmp = it;
    ++it;
    delete *it_tmp;
  }
  graph_changes_.clear();
}

void FlowGraphManager::UpdateTimeDependentCosts(vector<JobDescriptor*>* jobs) {
  for (auto& jd_ptr : *jobs) {
    VLOG(1) << "Reconsidering time-dependent costs for job " << jd_ptr->uuid();
    AddOrUpdateJobNodes(jd_ptr);
  }
}

void FlowGraphManager::UpdateUnscheduledAggArcCosts() {
  unordered_map<JobID_t, uint64_t,
                boost::hash<boost::uuids::uuid> >::iterator it =
      job_unsched_to_node_id_.begin();
  for (; it != job_unsched_to_node_id_.end(); ++it) {
    FlowGraphNode* unsched_node = flow_graph_->Node(it->second);
    for (unordered_map<uint64_t, FlowGraphArc*>::iterator
         ait = unsched_node->incoming_arc_map_.begin();
         ait != unsched_node->incoming_arc_map_.end();) {
      unordered_map<uint64_t, FlowGraphArc*>::iterator ait_tmp = ait;
      ++ait;
      FlowGraphArc* arc = ait_tmp->second;
      CHECK_NOTNULL(arc);
      TaskID_t task_id = flow_graph_->Node(arc->src_)->task_id_;

      Cost_t new_cost = cost_model_->TaskToUnscheduledAggCost(task_id);
      CHECK_GE(new_cost, 0);
      Cost_t old_cost = arc->cost_;
      flow_graph_->ChangeArcCost(arc, new_cost);
      DIMACSChange *chg = new DIMACSChangeArc(*arc, old_cost);
      chg->set_comment("UpdateUnscheduledAggArcCosts");
      dimacs_stats_->UpdateStats(CHG_ARC_TO_UNSCHED);
      AddGraphChange(chg);
    }
  }
}

void FlowGraphManager::UpdateUnscheduledAggToSinkCapacity(
    JobID_t job, int64_t delta) {
  uint64_t* unsched_agg_node_id = FindOrNull(job_unsched_to_node_id_, job);
  CHECK_NOTNULL(unsched_agg_node_id);
  FlowGraphArc* unsched_agg_to_sink_arc =
    FindPtrOrNull(flow_graph_->Node(*unsched_agg_node_id)->outgoing_arc_map_,
                  sink_node_->id_);
  CHECK_NOTNULL(unsched_agg_to_sink_arc);
  unsched_agg_to_sink_arc->cap_upper_bound_ += delta;
  DIMACSChange *chg = new DIMACSChangeArc(*unsched_agg_to_sink_arc,
                                          unsched_agg_to_sink_arc->cost_);
  chg->set_comment("UpdateUnscheduledAggToSinkCapacity");
  dimacs_stats_->UpdateStats(CHG_ARC_FROM_UNSCHED);
  AddGraphChange(chg);
}

}  // namespace firmament

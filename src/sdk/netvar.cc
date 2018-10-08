#include "precompiled.hh"

#include "interface.hh"
#include "netvar.hh"
#include "sdk.hh"

using namespace sdk;

static Netvar *head;

void Netvar::Tree::populate_recursive(RecvTable *t, TreeNode *nodes) {
    for (auto i = 0; i < t->prop_count; i++) {
        auto *     prop     = t->prop(i);
        const auto new_node = std::make_shared<Node>();
        new_node->p         = prop;

        if (prop->recv_type == 6) populate_recursive(prop->as_datatable(), &new_node->children);

        nodes->emplace_back(std::make_pair(prop->network_name, new_node));
    }
}

Netvar::Tree::Tree() {
}

void Netvar::Tree::init() {
    if (prop_tree.size() > 0) return;

    auto cc = iface::client->get_all_classes();
    while (cc != nullptr) {
        const auto new_node = std::make_shared<Node>();
        new_node->p         = nullptr;

        populate_recursive(cc->recv_table, &new_node->children);
        prop_tree.emplace_back(cc->recv_table->name, new_node);

        cc = cc->next;
    }
}

uptr Netvar::Tree::find_offset(std::vector<const char *> t) {
    uptr total = 0;
    auto nodes = &prop_tree;

    for (auto &name : t) {
        auto old_nodes = nodes;

        auto end = nodes->end();
        for (auto it = nodes->begin(); it != end; ++it) {
            auto p = *it;

            auto [node_name, child_node] = p;

            if (strcmp(name, node_name) == 0) {
                nodes = &child_node->children;
                if (child_node->p != nullptr)
                    total += child_node->p->offset;
                break;
            }
        }

        if (nodes == old_nodes) {
            // TODO:
            // logging::msg("[Netvar] Unable to find '%s'", name);
        }
    }

    return total;
}

Netvar::Tree Netvar::netvar_tree;

void Netvar::add_to_init_list() {
    next = head;
    head = this;
}

void Netvar::init() {
    offset = netvar_tree.find_offset(tables);
}

void Netvar::init_all() {
    netvar_tree.init();

    for (auto &n = head; n != nullptr; n = n->next) {
        n->init();
    }
}

/*
 * Copyright 2015 Cloudius Systems
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#define BOOST_TEST_DYN_LINK

#include <seastar/core/thread.hh>
#include <seastar/tests/test-utils.hh>
#include <seastar/util/defer.hh>

#include "tests/cql_test_env.hh"
#include "tests/mutation_source_test.hh"
#include "tests/result_set_assertions.hh"
#include "service/migration_manager.hh"
#include "schema_builder.hh"

SEASTAR_TEST_CASE(test_new_schema_with_no_structural_change_is_propagated) {
    return do_with_cql_env([](cql_test_env& e) {
        return seastar::async([&] {
            auto partial = schema_builder("tests", "table")
                    .with_column("pk", bytes_type, column_kind::partition_key)
                    .with_column("v1", bytes_type);

            e.execute_cql("create keyspace tests with replication = { 'class' : 'SimpleStrategy', 'replication_factor' : 1 };").get();

            auto old_schema = partial.build();

            service::get_local_migration_manager().announce_new_column_family(old_schema, false).get();

            auto old_table_version = e.db().local().find_schema(old_schema->id())->version();
            auto old_node_version = e.db().local().get_version();

            auto new_schema = partial.build();
            BOOST_REQUIRE_NE(new_schema->version(), old_schema->version());

            service::get_local_migration_manager().announce_column_family_update(new_schema, false).get();

            BOOST_REQUIRE_NE(e.db().local().find_schema(old_schema->id())->version(), old_table_version);
            BOOST_REQUIRE_NE(e.db().local().get_version(), old_node_version);
        });
    });
}

SEASTAR_TEST_CASE(test_tombstones_are_ignored_in_version_calculation) {
    return do_with_cql_env([](cql_test_env& e) {
        return seastar::async([&] {
            e.execute_cql("create keyspace tests with replication = { 'class' : 'SimpleStrategy', 'replication_factor' : 1 };").get();

            auto table_schema = schema_builder("ks", "table")
                    .with_column("pk", bytes_type, column_kind::partition_key)
                    .with_column("v1", bytes_type)
                    .build();

            service::get_local_migration_manager().announce_new_column_family(table_schema, false).get();

            auto old_table_version = e.db().local().find_schema(table_schema->id())->version();
            auto old_node_version = e.db().local().get_version();

            {
                BOOST_MESSAGE("Applying a no-op tombstone to v1 column definition");
                auto s = db::schema_tables::columns();
                auto pkey = partition_key::from_singular(*s, table_schema->ks_name());
                mutation m(pkey, s);
                auto ckey = clustering_key::from_exploded(*s, {utf8_type->decompose(table_schema->cf_name()), "v1"});
                m.partition().apply_delete(*s, ckey, tombstone(api::min_timestamp, gc_clock::now()));
                service::get_local_migration_manager().announce(std::vector<mutation>({m}), true).get();
            }

            auto new_table_version = e.db().local().find_schema(table_schema->id())->version();
            auto new_node_version = e.db().local().get_version();

            BOOST_REQUIRE_EQUAL(new_table_version, old_table_version);
            BOOST_REQUIRE_EQUAL(new_node_version, old_node_version);
        });
    });
}

SEASTAR_TEST_CASE(test_column_is_dropped) {
    return do_with_cql_env([](cql_test_env& e) {
        return seastar::async([&] {
            e.execute_cql("create keyspace tests with replication = { 'class' : 'SimpleStrategy', 'replication_factor' : 1 };").get();
            e.execute_cql("create table tests.table1 (pk int primary key, c1 int, c2 int);").get();
            e.execute_cql("alter table tests.table1 drop c2;").get();
            e.execute_cql("alter table tests.table1 add s1 int;").get();

            schema_ptr s = e.db().local().find_schema("tests", "table1");
            BOOST_REQUIRE(s->all_columns().count(to_bytes("c1")));
            BOOST_REQUIRE(!s->all_columns().count(to_bytes("c2")));
            BOOST_REQUIRE(s->all_columns().count(to_bytes("s1")));
        });
    });
}

class counting_migration_listener : public service::migration_listener {
public:
    int create_keyspace_count = 0;
    int create_column_family_count = 0;
    int create_user_type_count = 0;
    int create_function_count = 0;
    int create_aggregate_count = 0;
    int update_keyspace_count = 0;
    int update_column_family_count = 0;
    int columns_changed_count = 0;
    int update_user_type_count = 0;
    int update_function_count = 0;
    int update_aggregate_count = 0;
    int drop_keyspace_count = 0;
    int drop_column_family_count = 0;
    int drop_user_type_count = 0;
    int drop_function_count = 0;
    int drop_aggregate_count = 0;
public:
    virtual void on_create_keyspace(const sstring&) override { ++create_keyspace_count; }
    virtual void on_create_column_family(const sstring&, const sstring&) override { ++create_column_family_count; }
    virtual void on_create_user_type(const sstring&, const sstring&) override { ++create_user_type_count; }
    virtual void on_create_function(const sstring&, const sstring&) override { ++create_function_count; }
    virtual void on_create_aggregate(const sstring&, const sstring&) override { ++create_aggregate_count; }
    virtual void on_update_keyspace(const sstring&) override { ++update_keyspace_count; }
    virtual void on_update_column_family(const sstring&, const sstring&, bool columns_changed) override {
        ++update_column_family_count;
        columns_changed_count += int(columns_changed);
    }
    virtual void on_update_user_type(const sstring&, const sstring&) override { ++update_user_type_count; }
    virtual void on_update_function(const sstring&, const sstring&) override { ++update_function_count; }
    virtual void on_update_aggregate(const sstring&, const sstring&) override { ++update_aggregate_count; }
    virtual void on_drop_keyspace(const sstring&) override { ++drop_keyspace_count; }
    virtual void on_drop_column_family(const sstring&, const sstring&) override { ++drop_column_family_count; }
    virtual void on_drop_user_type(const sstring&, const sstring&) override { ++drop_user_type_count; }
    virtual void on_drop_function(const sstring&, const sstring&) override { ++drop_function_count; }
    virtual void on_drop_aggregate(const sstring&, const sstring&) override { ++drop_aggregate_count; }
};

SEASTAR_TEST_CASE(test_notifications) {
    return do_with_cql_env([](cql_test_env& e) {
        return seastar::async([&] {
            counting_migration_listener listener;
            service::get_local_migration_manager().register_listener(&listener);
            auto listener_lease = defer([&listener] { service::get_local_migration_manager().register_listener(&listener); });

            e.execute_cql("create keyspace tests with replication = { 'class' : 'SimpleStrategy', 'replication_factor' : 1 };").get();

            BOOST_REQUIRE_EQUAL(listener.create_keyspace_count, 1);

            e.execute_cql("create table tests.table1 (pk int primary key, c1 int, c2 int);").get();

            BOOST_REQUIRE_EQUAL(listener.create_column_family_count, 1);
            BOOST_REQUIRE_EQUAL(listener.columns_changed_count, 0);

            e.execute_cql("alter table tests.table1 drop c2;").get();

            BOOST_REQUIRE_EQUAL(listener.update_column_family_count, 1);
            BOOST_REQUIRE_EQUAL(listener.columns_changed_count, 1);

            e.execute_cql("alter table tests.table1 add s1 int;").get();

            BOOST_REQUIRE_EQUAL(listener.update_column_family_count, 2);
            BOOST_REQUIRE_EQUAL(listener.columns_changed_count, 2);

            e.execute_cql("alter table tests.table1 alter s1 type blob;").get();

            BOOST_REQUIRE_EQUAL(listener.update_column_family_count, 3);
            BOOST_REQUIRE_EQUAL(listener.columns_changed_count, 3);

            e.execute_cql("drop table tests.table1;").get();

            BOOST_REQUIRE_EQUAL(listener.drop_column_family_count, 1);
        });
    });
}

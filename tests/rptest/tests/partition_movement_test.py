# Copyright 2020 Vectorized, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

import random
import time

from ducktape.mark.resource import cluster
from ducktape.utils.util import wait_until
from rptest.clients.kafka_cat import KafkaCat
import requests

from rptest.clients.types import TopicSpec
from rptest.tests.redpanda_test import RedpandaTest
from rptest.services.admin import Admin
from kafka import KafkaProducer
from kafka import KafkaConsumer


class PartitionMovementTest(RedpandaTest):
    """
    Basic partition movement tests. Each test builds a number of topics and then
    performs a series of random replica set changes. After each change a
    verification step is performed.

    TODO
    - Add tests with node failures
    - Add tests with active producer/consumer
    - Add settings for scaling up tests
    - Add tests guarnateeing multiple segments
    """
    @staticmethod
    def _random_partition(metadata):
        topic = random.choice(metadata)
        partition = random.choice(topic["partitions"])
        return topic["topic"], partition["partition"]

    @staticmethod
    def _choose_replacement(admin, assignments):
        """
        Does not produce assignments that contain duplicate nodes. This is a
        limitation in redpanda raft implementation.
        """
        replication_factor = len(assignments)
        node_ids = lambda x: set([a["node_id"] for a in x])

        assert replication_factor >= 1
        assert len(node_ids(assignments)) == replication_factor

        # remove random assignment(s). we allow no changes to be made to
        # exercise the code paths responsible for dealing with no-ops.
        num_replacements = random.randint(0, replication_factor)
        selected = random.sample(assignments, num_replacements)
        for assignment in selected:
            assignments.remove(assignment)

        # choose a valid random replacement
        replacements = []
        brokers = admin.get_brokers()
        while len(assignments) != replication_factor:
            broker = random.choice(brokers)
            node_id = broker["node_id"]
            if node_id in node_ids(assignments):
                continue
            core = random.randint(0, broker["num_cores"] - 1)
            replacement = dict(node_id=node_id, core=core)
            assignments.append(replacement)
            replacements.append(replacement)

        return selected, replacements

    @staticmethod
    def _get_assignments(admin, topic, partition):
        res = admin.get_partitions(topic, partition)

        def normalize(a):
            return dict(node_id=a["node_id"], core=a["core"])

        return [normalize(a) for a in res["replicas"]]

    @staticmethod
    def _equal_assignments(r0, r1):
        def to_tuple(a):
            return a["node_id"], a["core"]

        r0 = [to_tuple(a) for a in r0]
        r1 = [to_tuple(a) for a in r1]
        return set(r0) == set(r1)

    def _get_current_partitions(self, admin, topic, partition_id):
        def keep(p):
            return p["ns"] == "kafka" and p["topic"] == topic and p[
                "partition_id"] == partition_id

        result = []
        for node in self.redpanda.nodes:
            node_id = self.redpanda.idx(node)
            partitions = admin.get_partitions(node=node)
            partitions = filter(keep, partitions)
            for partition in partitions:
                result.append(dict(node_id=node_id, core=partition["core"]))
        return result

    def _move_and_verify(self):
        admin = Admin(self.redpanda)

        # choose a random topic-partition
        metadata = self.redpanda.describe_topics()
        topic, partition = self._random_partition(metadata)
        self.logger.info(f"selected topic-partition: {topic}-{partition}")

        # get the partition's replica set, including core assignments. the kafka
        # api doesn't expose core information, so we use the redpanda admin api.
        assignments = self._get_assignments(admin, topic, partition)
        self.logger.info(f"assignments for {topic}-{partition}: {assignments}")

        # build new replica set by replacing a random assignment
        selected, replacements = self._choose_replacement(admin, assignments)
        self.logger.info(
            f"replacement for {topic}-{partition}:{len(selected)}: {selected} -> {replacements}"
        )
        self.logger.info(
            f"new assignments for {topic}-{partition}: {assignments}")

        admin.set_partition_replicas(topic, partition, assignments)

        def status_done():
            info = admin.get_partitions(topic, partition)
            self.logger.info(
                f"current assignments for {topic}-{partition}: {info}")
            converged = self._equal_assignments(info["replicas"], assignments)
            return converged and info["status"] == "done"

        # wait until redpanda reports complete
        wait_until(status_done, timeout_sec=30, backoff_sec=1)

        def derived_done():
            info = self._get_current_partitions(admin, topic, partition)
            self.logger.info(
                f"derived assignments for {topic}-{partition}: {info}")
            return self._equal_assignments(info, assignments)

        wait_until(derived_done, timeout_sec=30, backoff_sec=1)

    @cluster(num_nodes=3)
    def test_empty(self):
        """
        Move empty partitions.
        """
        topics = []
        for partition_count in range(1, 5):
            for replication_factor in (3, 3):
                name = f"topic{len(topics)}"
                spec = TopicSpec(name=name,
                                 partition_count=partition_count,
                                 replication_factor=replication_factor)
                topics.append(spec)

        for spec in topics:
            self.redpanda.create_topic(spec)

        for _ in range(25):
            self._move_and_verify()

    @cluster(num_nodes=3)
    def test_static(self):
        """
        Move partitions with data, but no active producers or consumers.
        """
        topics = []
        for partition_count in range(1, 5):
            for replication_factor in (3, 3):
                name = f"topic{len(topics)}"
                spec = TopicSpec(name=name,
                                 partition_count=partition_count,
                                 replication_factor=replication_factor)
                topics.append(spec)

        for spec in topics:
            self.redpanda.create_topic(spec)

        num_records = 1000
        produced = set(((f"key-{i}".encode(), f"value-{i}".encode())
                        for i in range(num_records)))

        for spec in topics:
            self.logger.info(f"Producing to {spec}")
            producer = KafkaProducer(
                batch_size=4096,
                bootstrap_servers=self.redpanda.brokers_list())
            for key, value in produced:
                producer.send(spec.name, key=key, value=value)
            producer.flush()
            self.logger.info(f"Finished producing to {spec}")

        for _ in range(25):
            self._move_and_verify()

        for spec in topics:
            self.logger.info(f"Verifying records in {spec}")
            consumer = KafkaConsumer(
                spec.name,
                bootstrap_servers=self.redpanda.brokers_list(),
                group_id=None,
                auto_offset_reset='earliest',
                request_timeout_ms=5000,
                consumer_timeout_ms=10000)
            consumed = []
            for msg in consumer:
                consumed.append((msg.key, msg.value))
            self.logger.info(f"Finished verifying records in {spec}")
            assert set(consumed) == produced

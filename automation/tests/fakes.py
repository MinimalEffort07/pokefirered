"""A stand-in for the `gh` adapter that replays recorded GraphQL responses.

Fixtures are JSON files in tests/fixtures/. Each one maps a GraphQL
operation name (the word after `query` in the request, e.g. `FindBoard`)
to the response GitHub sent back. Paged operations map the page cursor
they were asked for ("" for the first page) to that page's response.

Every request is recorded, so a test can prove the tick only read from
GitHub: `mutations` lists any request that would have changed something.
"""

import json
import os
import re

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")

_OPERATION = re.compile(r"^\s*(query|mutation)\s+(\w+)")


class FakeGh:
    def __init__(self, responses):
        self.responses = responses
        self.requests = []

    @classmethod
    def from_fixture(cls, name):
        with open(os.path.join(FIXTURES, name)) as f:
            return cls(json.load(f))

    def graphql(self, query, **variables):
        match = _OPERATION.match(query)
        if not match:
            raise AssertionError("request has no operation name: " + query[:60])
        kind, name = match.groups()
        self.requests.append((kind, name, variables))
        if kind == "mutation":
            return {"data": {}}
        response = self.responses[name]
        if "cursor" in variables:
            response = response[variables["cursor"] or ""]
        return response

    @property
    def mutations(self):
        return [r for r in self.requests if r[0] == "mutation"]

// |reftest| skip-if(!this.hasOwnProperty('Temporal')) -- Temporal is not enabled unconditionally
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.calendar.prototype.monthsinyear
description: Time separator in string argument can vary
features: [Temporal]
---*/

const tests = [
  ["2000-05-02T15:23", "uppercase T"],
  ["2000-05-02t15:23", "lowercase T"],
  ["2000-05-02 15:23", "space between date and time"],
];

const instance = new Temporal.Calendar("iso8601");

tests.forEach(([arg, description]) => {
  const result = instance.monthsInYear(arg);

  assert.sameValue(
    result,
    12,
    `variant time separators (${description})`
  );
});

reportCompare(0, 0);

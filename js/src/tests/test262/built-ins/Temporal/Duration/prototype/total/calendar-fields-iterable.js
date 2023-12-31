// |reftest| skip-if(!this.hasOwnProperty('Temporal')) -- Temporal is not enabled unconditionally
// Copyright (C) 2021 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.duration.prototype.total
description: Verify the result of calendar.fields() is treated correctly.
info: |
    sec-temporal.duration.prototype.total step 4:
      4. Let _relativeTo_ be ? ToRelativeTemporalObject(_options_).
    sec-temporal-torelativetemporalobject step 4.c:
      c. Let _fieldNames_ be ? CalendarFields(_calendar_, « *"day"*, *"month"*, *"monthCode"*, *"year"* »).
    sec-temporal-calendarfields step 4:
      4. Let _result_ be ? IterableToList(_fieldsArray_).
includes: [compareArray.js, temporalHelpers.js]
features: [Temporal]
---*/

const expected = [
  "day",
  "hour",
  "microsecond",
  "millisecond",
  "minute",
  "month",
  "monthCode",
  "nanosecond",
  "second",
  "year",
];

const calendar = TemporalHelpers.calendarFieldsIterable();
const duration = new Temporal.Duration(1, 1, 1, 1, 1, 1, 1);
duration.total({ unit: 'seconds', relativeTo: { year: 2000, month: 1, day: 1, calendar } });

assert.sameValue(calendar.fieldsCallCount, 1, "fields() method called once");
assert.compareArray(calendar.fieldsCalledWith[0], expected, "fields() method called with correct args");
assert(calendar.iteratorExhausted[0], "iterated through the whole iterable");

reportCompare(0, 0);

// |reftest| skip-if(!this.hasOwnProperty('Temporal')) -- Temporal is not enabled unconditionally
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plaindate.prototype.toplainmonthday
description: >
    Calendar.monthDayFromFields method is called with undefined as the options
    value when call originates internally
includes: [temporalHelpers.js]
features: [Temporal]
---*/

const calendar = TemporalHelpers.calendarFromFieldsUndefinedOptions();
const instance = new Temporal.PlainDate(2000, 5, 2, calendar);
instance.toPlainMonthDay();
assert.sameValue(calendar.monthDayFromFieldsCallCount, 1);

reportCompare(0, 0);

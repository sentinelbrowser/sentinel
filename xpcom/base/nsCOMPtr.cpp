/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCOMPtr.h"

nsresult nsQueryInterfaceISupports::operator()(const nsIID& aIID,
                                               void** aAnswer) const {
  nsresult status;
  if (mRawPtr) {
    status = mRawPtr->QueryInterface(aIID, aAnswer);
  } else {
    status = NS_ERROR_NULL_POINTER;
  }

  return status;
}

nsresult nsQueryInterfaceISupportsWithError::operator()(const nsIID& aIID,
                                                        void** aAnswer) const {
  nsresult status;
  if (mRawPtr) {
    status = mRawPtr->QueryInterface(aIID, aAnswer);
  } else {
    status = NS_ERROR_NULL_POINTER;
  }

  if (mErrorPtr) {
    *mErrorPtr = status;
  }
  return status;
}

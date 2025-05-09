//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Contributors to the OpenEXR Project.
//

//-----------------------------------------------------------------------------
//
//	class Semaphore -- dummy implementation for
//	for platforms that do not support threading
//
//-----------------------------------------------------------------------------

#include "IlmThreadSemaphore.h"

#if ILMTHREAD_SEMAPHORE_DISABLED

ILMTHREAD_INTERNAL_NAMESPACE_SOURCE_ENTER


Semaphore::Semaphore (unsigned int value) {}
Semaphore::~Semaphore () {}
void Semaphore::wait () {}
bool Semaphore::tryWait () {return true;}
void Semaphore::post () {}
int Semaphore::value () const {return 0;}


ILMTHREAD_INTERNAL_NAMESPACE_SOURCE_EXIT

#endif // ILMTHREAD_SEMAPHORE_DISABLED

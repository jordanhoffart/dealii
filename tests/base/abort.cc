//-----------------------------------------------------------------------
//    $Id$
//    Version: $Name$
//
//    Copyright (C) 1998, 1999, 2000, 2001, 2002 by the deal.II authors
//
//    This file is subject to QPL and may not be  distributed
//    without copyright and license information. Please refer
//    to the file deal.II/doc/license.html for the  text  and
//    further information on this license.
//
//-----------------------------------------------------------------------


// when linking with the object file generated from this file, calling
// aborts() will not abort the program, but simply resume
// operation. while usually useless, this is used in the test to
// generate the output of a triggered assertion without aborting the
// program. we do this, since we sometimes want to test that some code
// actually generates an assertion, which would otherwise be
// impossible

extern "C"
void abort()
{}

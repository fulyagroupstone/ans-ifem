IFEM source code
================

Copyright (C) 2014 by 
Luca Heltai (1), Saswati Roy (2), and Francesco Costanzo (3)

(1) Scuola Internazionale Superiore di Studi Avanzati
    E-mail: luca.heltai@sissa.it
(2) Center for Neural Engineering, The Pennsylvania State University
    E-Mail: sur164@psu.edu
(3) Center for Neural Engineering, The Pennsylvania State University
    E-Mail: costanzo@engr.psu.edu

This code was developed starting from the example
step-33 of the deal.II FEM library, and it is the companion code to
the article 

"A Fully Coupled Immersed Finite Element Method for Fluid 
Structure Interaction via the Deal.II Library", 
Archive of Numerical Software, 2014, 2(1), pag. 1--27.
 
This file is subject to LGPL version 2.1 or later and may not be
distributed without copyright and license information. Please refer to
section 5 and 6 of this file for further information on this license.

1. Deal.II Requirements:
========================

The FEIBM source code requires the deal.II 8.4 library or greater. It
has also been tested with the current git release deal.II 8.5.pre.

In what follows, we assume that the user has installed the deal.II
library in the directory

	DEAL_II_DIR

and that the user has defined the enviroment variable DEAL_II_DIR to
point to the correct location. If this enviroment variable is not set,
the user should specify it by hand when running cmake in order for
cmake to properly locate the deal.II library. For the program to work
properly, deal.II should be configured with support for UMFPACK.
  
2. Installation procedure:
==========================

The provided archive should be unzipped in a dedicated 
subdirectory, with the commands

	cd PATH_WHERE_YOU_WANT_THIS_CODE
	tar xvfz ans-ifem-vXXX.tgz
	
or cloned using git

	git clone https://github.com/luca-heltai/ans-ifem

The program can then be compiled by running

	mkdir build
	cd build
	cmake -DDEAL_II_DIR=/path/to/deal.II ..
	make
	
3. Running instructions:
========================

Once the program has been compiled, it can be run by typing 

	./ifem

or 

	./ifem parameters.prm

in the directory ans-ifem.
The program uses parameter files to set its runtime variables. The
file 

	immersed_fem.prm 

is an example of such files, and it is the default one used if none is
specified at run time. If the specified file does not exists, the
program will attempt to create one with default values for you. 
The directory 

	prms/

contains all parameter files used to produce the results presented in
the paper.

4. Extensive documentation:
===========================

If the user has the program Doxygen installed, a complete and
browsable documentation of the source code itself can be generated by
setting the cmake varibale BUILD_DOCUMENTATION=ON

	cmake -DBUILD_DOCUMENTATION=ON -DDEAL_II_DIR=/path/to/deal.II .

and then running

	make docs

In this case, the documentation will be accessible in the subdirectory

	./doc/html

If the user wants the deal.II documentation to be inlined, then the
file http://www.dealii.org/developer/doxygen/deal.tag should be
downloaded to the program directory before making the documentation,
for example, usgin wget

	wget http://www.dealii.org/developer/doxygen/deal.tag
	make docs

5. Licence Informations
=======================

The step-feibm library has been placed under an Open Source license,
in the sense advocated by the Open Source Initiative.

However, this program is not in the public domain, it is property of
and copyrighted by Luca Heltai, Francesco Costanzo and Saswati Roy,
and there are restrictions on its use: The legally binding license is
the GNU Lesser General Public License (LGPL) as published by the Free
Software Foundation; either version 2.1 of the License, or (at your
option) any later version.

See the file LICENCE in this directory for more information.

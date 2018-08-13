# This file is a somewhat generic Makefile for my C projects

# Copyright (C) <2018> Jose Maria Perez Ramos

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.    See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.    If not, see <http://www.gnu.org/licenses/>.

# Author: Jose Maria Perez Ramos <jose.m.perez.ramos+git gmail>
# Date: 2018.08.13
# Version: 1.0.0

NAME     = term_pa_spectrum
LDFLAGS  = -lfftw3 -lm -lpulse
BUILDDIR = build
SRCDIR   = src
CFLAGS   = -Wall

SRC = $(wildcard $(SRCDIR)/*.c)

OBJ = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRC))
DEP = $(OBJ:.o=.d)


$(NAME): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ): | build_dir
build_dir:
	@mkdir -p $(BUILDDIR)


### DEPS ####################################
# Generate dependency files and include them
# This will make .o depend on all included .h
$(BUILDDIR)/%.d: $(SRCDIR)/%.c
	@echo "Generating dep: $@"
	@$(CPP) $(CPPFLAGS) $< -MM -MT $(@:.d=.o) > $@

$(DEP): | build_dir

-include $(DEP)
#############################################

run: $(NAME)
	-./$(NAME)

.PHONY: clean
clean:
	rm -rf $(BUILDDIR) $(NAME)


# Careful, it does not detect the changes in
# defined flags (TODO)
.PHONY: debug
debug: CFLAGS += -DDEBUG -g
debug: $(NAME)


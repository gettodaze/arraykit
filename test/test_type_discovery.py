



import unittest
import typing as tp
from enum import Enum


'''
Discovery

options:
is bool:
    all values are case insensitive "true" or "false"
    genfromtxt, dtype None: requires no leading space, ignores trailing space
    genfromtxt, dtype bool: requires four character true with no leading space
    AK: can permit leading spaces
is int:
    all values are numbers
    might start with "+" or "-"
    might permit comma
is float:
    all values are numbers except
    might have one "e" or "E" between numbers (cannot start or end)
    might have one ".", can lead or trail
    can have "nan"
is complex:
    all values are numbers except
    must have one "j" or "J", cannot lead, can trail; if used as delimiter, must be followed by sign
    might have one or two "+", "-", leading or after j
    might be surrounded in parenthesis
    note: genfromtxt only identifies if 'j' is in trailing position, 1+3j, not 3j+1
is string:
    any character other than e, j, n, a, or not true/false sequence
is empty:
    only space
    if there is another float, will be interpreted as NaN
    if there are only integers, will be interpreted as -1
    if combined with False/True, will be interpreted as str
'''

'''
Discover contiguous numeric, i.e., if contiguous sequence of digits, e, j, sign,decimal; then, after parse complete, look at e/j/decimal counts to determine numeric type
'''


# functions as needed in C implementation
def is_digit(c: str) -> bool:
    #define isdigit_ascii(c) (((unsigned)(c) - '0') < 10u)
    return c.isdigit()

def is_space(c: str) -> bool:
    #define isspace_ascii(c) (((c) == ' ') || (((unsigned)(c) - '\t') < 5))
    return c.isspace()

def is_sign(c: str) -> bool:
    return c == '+' or c == '-'

def is_paren_open(c: str) -> bool:
    return c == '('

def is_paren_close(c: str) -> bool:
    return c == ')'

def is_decimal(c: str) -> bool:
    return c == '.'

# def ismatch(c: str, match: str) -> str:
#     '''Do a case insensitive character match, given the upper case character.
#     '''
#     # can go from upper to lower with | 0x20, to upper with & 0x5f
#     assert ord(match) <= 90 # must be upper case
#     return c == match or c == chr(ord(match) | 0x20)

def is_a(c: str) -> bool:
    return c == 'a' or c == 'A'

def is_e(c: str) -> bool:
    return c == 'e' or c == 'E'

def is_f(c: str) -> bool:
    return c == 'f' or c == 'F'

def is_j(c: str) -> bool:
    return c == 'j' or c == 'J'

def is_l(c: str) -> bool:
    return c == 'l' or c == 'L'

def is_n(c: str) -> bool:
    return c == 'n' or c == 'N'

def is_r(c: str) -> bool:
    return c == 'r' or c == 'R'

def is_s(c: str) -> bool:
    return c == 's' or c == 'S'

def is_t(c: str) -> bool:
    return c == 't' or c == 'T'

def is_u(c: str) -> bool:
    return c == 'u' or c == 'U'


class TypeResolved(Enum):
    IS_UNKNOWN = 1
    IS_BOOL = 2
    IS_INT = 3
    IS_FLOAT = 4
    IS_COMPLEX = 5
    IS_STRING = 6
    IS_EMPTY = 7

class TypeField:
    '''
    Estimate the type of a field. This estimate can be based on character type counts. Some ordering considerations will be ignored for convenience; if downstream parsing fails, fallback will be to a string type anyway.
    '''
    def __init__(self) -> None:
        self.reset()
        self.resolved_line: TypeResolved = TypeResolved.IS_UNKNOWN

    def reset(self) -> None:
        self.resolved_field: TypeResolved = TypeResolved.IS_UNKNOWN

        self.previous_leading_space = False
        self.previous_numeric = False
        self.contiguous_numeric = False

        self.count_leading_space = 0
        self.count_bool = 0 # signed, not greater than +/- 5

        # numeric symbols; values do not need to be greater than 4
        self.count_sign = 0
        self.count_e = 0
        self.count_j = 0
        self.count_decimal = 0
        self.count_nan = 0
        self.count_paren_open = 0
        self.count_paren_close = 0

        # can be unbound in size
        self.count_digit = 0
        self.count_notspace = 0 # non-digit, non-space

    def process_char(self, c: str, pos: int) -> int:
        # position is postion needs to be  dropping leading space
        # update self based on c and position
        # return int where 1 means process more, 0 means stop, -1 means error

        if self.resolved_field != TypeResolved.IS_UNKNOWN:
            return 0

        # evaluate space -------------------------------------------------------
        space = False

        if is_space(c):
            if pos == 0:
                self.previous_leading_space = True

            if self.previous_leading_space:
                self.count_leading_space += 1
                return 1
            space = True
        else:
            self.count_notspace += 1


        self.previous_leading_space = False # this char is not space

        pos_field = pos - self.count_leading_space

        # evaluate numeric, non-positional -------------------------------------
        numeric = False
        digit = False

        if space:
            pass

        elif is_digit(c):
            numeric = True
            digit = True
            self.count_digit += 1

        elif is_sign(c):
            self.count_sign += 1
            if self.count_sign > 4:
                # complex numbers with E can have up to 4 signs, anything else is a string
                self.resolved_field = TypeResolved.IS_STRING
                return 0
            numeric = True

        elif is_paren_open(c):
            numeric = True
            self.count_paren_open += 1
            # open paren only permitted at pos_field 0
            if pos_field != 0 or self.count_paren_open > 1:
                self.resolved_field = TypeResolved.IS_STRING
                return 0

        elif is_paren_close(c):
            numeric = True
            self.count_paren_close += 1
            # NOTE: not evaluating that this is on last position of contiguous numeric
            if self.count_paren_close > 1:
                self.resolved_field = TypeResolved.IS_STRING
                return 0

        elif is_e(c): # only character that is numeric and bool
            numeric = True
            self.count_e += 1
            if pos_field == 0 or self.count_e > 2:
                # true and false each only have one E, complex can have 2
                self.resolved_field = TypeResolved.IS_STRING
                return 0

        elif is_j(c):
            numeric = True
            self.count_j += 1
            if pos_field == 0 or self.count_j > 1:
                self.resolved_field = TypeResolved.IS_STRING
                return 0

        elif is_decimal(c):
            numeric = True
            self.count_decimal += 1
            if self.count_decimal > 2: # complex can have 2!
                self.resolved_field = TypeResolved.IS_STRING
                return 0

        #-----------------------------------------------------------------------
        # print(f' pre: {c=} {pos=} {pos_field=} {numeric=} {self.previous_numeric=} {self.contiguous_numeric=}')

        if numeric:
            if pos_field == 0:
                self.contiguous_numeric = True
                self.previous_numeric = True
                return 1 # E can not be in first position

            # pos_field > 0
            if not self.previous_numeric:
                # found a numeric not in pos 0 where previous was not numeric
                self.contiguous_numeric = False

            self.previous_numeric = True

            # NOTE: we need to consider possible Boolean scenario
            if self.contiguous_numeric or not is_e(c):
                return 1
        else: # not numeric, could be space or notspace
            if self.contiguous_numeric and not space:
                # if we find a non-numeric, non-space, after contiguous numeric
                self.resolved_field = TypeResolved.IS_STRING
                return 0
            self.previous_numeric = False


        # evaluate character positions -----------------------------------------
        if space or digit:
            return 1

        # print(f'post: {c=} {pos=} {pos_field=} {numeric=} {self.previous_numeric=} {self.contiguous_numeric=} {self.count_notspace=}')

        if pos_field == 0:
            if is_t(c):
                self.count_bool += 1
            elif is_f(c):
                self.count_bool -= 1
            elif is_n(c):
                self.count_nan += 1

        elif pos_field == 1:
            if is_r(c):
                self.count_bool += 1
            elif is_a(c):
                self.count_bool -= 1
                self.count_nan += 1

        elif pos_field == 2:
            if is_u(c):
                self.count_bool += 1
            elif is_l(c):
                self.count_bool -= 1
            elif is_n(c):
                self.count_nan += 1

        elif pos_field == 3:
            if is_e(c):
                self.count_bool += 1
            if is_s(c):
                self.count_bool -= 1
        elif pos_field == 4:
            if is_e(c) and self.count_bool == -4:
                self.count_bool -= 1

        return 1

    def resolve_field_type(self, count: int) -> None:
        '''
        As process char may abort early, provide final evaluation full count
        '''
        if count == 0:
            return TypeResolved.IS_EMPTY

        if self.resolved_field != TypeResolved.IS_UNKNOWN:
            return self.resolved_field
        if self.count_bool == 4 and self.count_notspace == 4:
            return TypeResolved.IS_BOOL
        if self.count_bool == -5 and self.count_notspace == 5:
            return TypeResolved.IS_BOOL
        if self.count_nan == 3 and self.count_notspace == 3:
            return TypeResolved.IS_FLOAT

        # determine
        if self.contiguous_numeric:
            # NOTE: have already handled cases with excessive counts
            if self.count_digit == 0:
                # can have contiguous numerics like +ej.- but no digits
                return TypeResolved.IS_STRING

            if (self.count_j == 0
                    and self.count_e == 0
                    and self.count_decimal == 0
                    and self.count_paren_open == 0
                    and self.count_paren_close == 0):
                return TypeResolved.IS_INT

            if (self.count_j == 0
                    and self.count_paren_open == 0
                    and self.count_paren_close == 0
                    and (self.count_decimal == 1 or self.count_e == 1)):
                return TypeResolved.IS_FLOAT

            if self.count_j == 1 and (
                    (self.count_paren_open == 1 and self.count_paren_close == 1)
                    or (self.count_paren_open == 0 and self.count_paren_close == 0)
                    ):
                return TypeResolved.IS_COMPLEX
            # if only paren and digits, mark as complex
            if self.count_j == 0 and (
                    (self.count_paren_open == 1 and self.count_paren_close == 1)
                    ):
                return TypeResolved.IS_COMPLEX

        return TypeResolved.IS_STRING

    @staticmethod
    def resolve_line_type(previous: TypeResolved, new: TypeResolved) -> None:
        if previous is TypeResolved.IS_UNKNOWN:
            return new

        # a string with anything else is a string
        if previous is TypeResolved.IS_STRING or new is TypeResolved.IS_STRING:
            return TypeResolved.IS_STRING

        if previous is TypeResolved.IS_BOOL:
            if new is TypeResolved.IS_BOOL:
                return TypeResolved.IS_BOOL
            else: # bool found with anything else is a string
                return TypeResolved.IS_STRING

        if previous is TypeResolved.IS_INT:
            if (new is TypeResolved.IS_EMPTY
                    or new is TypeResolved.IS_INT):
                return TypeResolved.IS_INT
            if new is TypeResolved.IS_FLOAT:
                return TypeResolved.IS_FLOAT
            if new is TypeResolved.IS_COMPLEX:
                return TypeResolved.IS_COMPLEX

        if previous is TypeResolved.IS_FLOAT:
            if (new is TypeResolved.IS_EMPTY
                    or new is TypeResolved.IS_INT
                    or new is TypeResolved.IS_FLOAT):
                return TypeResolved.IS_FLOAT
            if new is TypeResolved.IS_COMPLEX:
                return TypeResolved.IS_COMPLEX

        if previous is TypeResolved.IS_COMPLEX:
            if (new is TypeResolved.IS_EMPTY
                    or new is TypeResolved.IS_INT
                    or new is TypeResolved.IS_FLOAT
                    or new is TypeResolved.IS_COMPLEX):
                return TypeResolved.IS_COMPLEX

        raise NotImplementedError(previous, new)

    def process(self, field: str) -> TypeResolved:
        # print(f'process: {field=}')

        self.reset() # does not reset resolved_line
        pos = 0
        continue_process = 1
        for char in field:
            if continue_process:
                continue_process = self.process_char(char, pos)
            pos += 1 # results in count

        # must call after all chars processed, does not set self.resolved field
        rlt_new = self.resolve_field_type(pos)
        self.resolved_line = self.resolve_line_type(self.resolved_line, rlt_new)
        # print(f'{self.resolved_line=}')
        return self.resolved_line

    def process_line(self, fields: tp.Iterable[str]) -> TypeResolved:
        for field in fields:
            self.process(field)
        return self.resolved_line


class TestUnit(unittest.TestCase):

    def test_bool_a(self) -> None:
        self.assertEqual(TypeField().process('   true'), TypeResolved.IS_BOOL)
        self.assertEqual(TypeField().process('FALSE'), TypeResolved.IS_BOOL)
        self.assertEqual(TypeField().process('FaLSE   '), TypeResolved.IS_BOOL)

        self.assertEqual(TypeField().process('  tals  '), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('FALSEblah'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('   true f'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('   true3'), TypeResolved.IS_STRING)

    def test_bool_b(self) -> None:
        self.assertEqual(TypeField().process('   true +'), TypeResolved.IS_STRING)


    def test_str_a(self) -> None:
        self.assertEqual(TypeField().process('+++'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('   ee   '), TypeResolved.IS_STRING)

    def test_int_a(self) -> None:
        self.assertEqual(TypeField().process(' 3'), TypeResolved.IS_INT)
        self.assertEqual(TypeField().process('3 '), TypeResolved.IS_INT)
        self.assertEqual(TypeField().process('  +3 '), TypeResolved.IS_INT)
        self.assertEqual(TypeField().process('+599w'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('k599'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('59 4'), TypeResolved.IS_STRING)

        self.assertEqual(TypeField().process('153'), TypeResolved.IS_INT)
        self.assertEqual(TypeField().process('  153  '), TypeResolved.IS_INT)
        self.assertEqual(TypeField().process('  15 3'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('5 3'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process(' 5 3 '), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('  5 3 '), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('  5  3 '), TypeResolved.IS_STRING)

    def test_float_a(self) -> None:
        self.assertEqual(TypeField().process(' .3'), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process('3. '), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process(' 2343. '), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process(' 2343.9 '), TypeResolved.IS_FLOAT)

        self.assertEqual(TypeField().process(' 23t3.9 '), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process(' 233.9!'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('4.3.5'), TypeResolved.IS_STRING)

    def test_float_b(self) -> None:
        self.assertEqual(TypeField().process(' 4e3'), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process('4E3 '), TypeResolved.IS_FLOAT)

        self.assertEqual(TypeField().process(' 4e3e'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('4e3   e'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('e99   '), TypeResolved.IS_STRING)

    def test_float_c(self) -> None:
        self.assertEqual(TypeField().process('  .  '), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('..'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('e+j.'), TypeResolved.IS_STRING)

    def test_float_d(self) -> None:
        self.assertEqual(TypeField().process('  nan'), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process('NaN   '), TypeResolved.IS_FLOAT)

        self.assertEqual(TypeField().process('NaN3   '), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process(' N an   '), TypeResolved.IS_STRING)


    def test_float_known_false_positive(self) -> None:
        # NOTE: we mark this as float because we do not observe that a number must follow e; assume this will fail in float conversion
        self.assertEqual(TypeField().process('8e'), TypeResolved.IS_FLOAT)

    def test_complex_a(self) -> None:
        self.assertEqual(TypeField().process('23j  '), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process(' 4e3j'), TypeResolved.IS_COMPLEX)

        self.assertEqual(TypeField().process(' 4e3jw'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process(' J4e3j'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('-4.3+3j'), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process(' j4e3'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process('j11111    '), TypeResolved.IS_STRING)

    def test_complex_b(self) -> None:
        self.assertEqual(TypeField().process('2.3-3.5j  '), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process('+23-35j  '), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process('+23-3.5j  '), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process('-3e-10-3e-2j'), TypeResolved.IS_COMPLEX)

        self.assertEqual(TypeField().process('+23-3.5j  +'), TypeResolved.IS_STRING)

    def test_complex_c(self) -> None:
        self.assertEqual(TypeField().process(' (23+3j) '), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process('(4e3-4.5j)'), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process('(4.3)'), TypeResolved.IS_COMPLEX)

        self.assertEqual(TypeField().process(' (23+3j)) '), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process(' (((23+3j'), TypeResolved.IS_STRING)
        self.assertEqual(TypeField().process(' 2(3+3j) '), TypeResolved.IS_STRING)


    def test_complex_known_false_positive(self) -> None:
        # NOTE: genfromtxt identifies this as string as j component is in first position
        self.assertEqual(TypeField().process('23j-43'), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process('+23-3.5j3'), TypeResolved.IS_COMPLEX)
        self.assertEqual(TypeField().process('(23+)3j '), TypeResolved.IS_COMPLEX)


    def test_line_a(self) -> None:
        self.assertEqual(TypeField().process_line(('25', '2.5', '')), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process_line((' .1', '2.5', '')), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process_line(('25', '', '')), TypeResolved.IS_INT)

        self.assertEqual(TypeField().process_line(('25', '2.5', 'e')), TypeResolved.IS_STRING)

    def test_line_b(self) -> None:
        self.assertEqual(TypeField().process_line(('  true', '  false', 'FALSE')), TypeResolved.IS_BOOL)
        self.assertEqual(TypeField().process_line(('  true', '  false', 'FALSEq')), TypeResolved.IS_STRING)

    def test_line_c(self) -> None:
        self.assertEqual(TypeField().process_line(('3', '', '4')), TypeResolved.IS_INT)

        self.assertEqual(TypeField().process_line(('3', '', '4e')), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process_line(('3', '', '.')), TypeResolved.IS_STRING)

    def test_line_d(self) -> None:
        self.assertEqual(TypeField().process_line(('3', '', '4.')), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process_line(('3', '', '4e3')), TypeResolved.IS_FLOAT)
        self.assertEqual(TypeField().process_line(('3', '', '(4e3)')), TypeResolved.IS_COMPLEX)


if __name__ == '__main__':
    unittest.main()




-- Negative test: P3-7.17 case statement with A'Range choice
procedure Neg_Case_Range_Attr is
   A : array (1 .. 3) of Integer := (1, 2, 3);
   X : Integer := 1;
begin
   case X is
      when A'Range => null;
      when others  => null;
   end case;
end Neg_Case_Range_Attr;

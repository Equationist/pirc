-- Negative test: P3-7.16 subtype range attribute type mismatch (Integer range Color_Arr'Range)
procedure Neg_Range_Attr_Mismatch is
   type Color is (Red, Green, Blue);
   type C_Arr is array (Color) of Integer;
   A : C_Arr := (others => 0);
   subtype Bad_I is Integer range A'Range;
begin
   null;
end Neg_Range_Attr_Mismatch;

-- Negative test: P3-7.14 uncompleted incomplete type as generic actual
procedure Neg_Gen_Incomplete_Actual is
   generic
      type T is limited private;
   package G is end G;
   type Inc;
   package I is new G (Inc);
   type Inc is new Integer;
begin
   null;
end Neg_Gen_Incomplete_Actual;

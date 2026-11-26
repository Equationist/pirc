-- Negative test: P3-6.8 non-object expression prefix for 'Address
with SYSTEM;
procedure Neg_Non_Obj_Addr is
   A : SYSTEM.Address := (1 + 2)'Address;
begin
   null;
end Neg_Non_Obj_Addr;

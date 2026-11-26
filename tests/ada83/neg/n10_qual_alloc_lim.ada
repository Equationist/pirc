-- Negative test: P3-7.12 qualified allocator on limited type & duplicate enum lit in private
procedure Neg_Qual_Alloc_Lim is
   task type T;
   task body T is begin null; end T;
   type TA is access T;
   X : T;
   P : TA := new T'(X);
begin
   null;
end Neg_Qual_Alloc_Lim;
